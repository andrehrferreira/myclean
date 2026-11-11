#include "core/tray_model/poller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/alerts.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/ipc/security.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/screens.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;
namespace metric = telemetry::metric;

// The parsed answer of `method`; nullopt when the call or the parsing failed.
template <typename T>
std::optional<T> fetch(IServiceLink& link, std::string_view method, const Json& params,
                       Result<T> (*parse)(const Json&)) {
  const Result<Json> reply = link.call(method, params);
  if (!reply) {
    return std::nullopt;
  }
  Result<T> parsed = parse(*reply);
  return parsed ? std::optional<T>(std::move(*parsed)) : std::nullopt;
}

}  // namespace

std::chrono::seconds range_length(Range range, std::chrono::seconds live) {
  switch (range) {
    case Range::live:
      break;
    case Range::hour:
      return std::chrono::hours{1};
    case Range::day:
      return std::chrono::days{1};
    case Range::month:
      return std::chrono::days{30};
  }
  return live;
}

bool charted(const SeriesKey& key) {
  static constexpr std::array<std::string_view, 10> system_lines = {
      metric::cpu_utility,       metric::cpu_dpc,         metric::cpu_interrupt,
      metric::memory_available,  metric::memory_commit,   metric::memory_commit_limit,
      metric::memory_standby,    metric::memory_modified, metric::memory_compressed,
      metric::memory_hard_faults};
  if (key.entity == metric::system) {
    return std::ranges::find(system_lines, key.metric) != system_lines.end();
  }
  return key.metric == metric::gpu_utilization || key.metric.starts_with("disk.");
}

PollResult Poller::poll(WallTime now, bool dashboard_open, const std::vector<Request>& requests) {
  PollResult result;
  result.time = now;
  select(dashboard_open, requests);
  const Result<Json> reply = link_->call(ipc::metrics_latest_method, Json::object());
  if (tasks_ != nullptr) {
    result.cleanup.task_activity = tasks_->activity();
  }
  if (!link_->reachable()) {
    connected_ = false;
    result.error = reply ? std::string() : reply.error().message;
    if (incidents_ != nullptr) {
      result.incidents = incidents_->counts();
    }
    return result;
  }
  if (!connected_) {
    // Connected (again): everything the service holds may have changed meanwhile.
    connected_ = true;
    next_slow_.reset();
    next_plans_.reset();
    badge_.reset();
  }
  if (reply) {
    if (Result<LatestReply> latest = parse_metrics_latest(*reply)) {
      fill_gaps(*latest, dashboard_open, result);
      result.latest = std::move(*latest);
    }
  }
  ask_again(dashboard_open, requests, result);
  bool refresh_plans = false;
  for (const Request& request : requests) {
    handle(request, result);
    refresh_plans = handle_cleanup(request, result) || refresh_plans;
    handle_governor(request, result);
    refresh_plans = handle_screens(request, result) || refresh_plans;
  }
  fetch_rest(now, dashboard_open, result);
  fetch_governor(result);
  report_windows(now);
  fetch_cleanup(now, dashboard_open, refresh_plans, result);
  run_tasks(result);
  // The service may have gone away after the first call.
  result.reachable = link_->reachable();
  if (!result.reachable) {
    connected_ = false;
    result.error = "the connection to the service broke";
  }
  return result;
}

void Poller::select(bool dashboard_open, const std::vector<Request>& requests) {
  if (dashboard_open && !dashboard_open_) {
    // Opening the dashboard shows current data at once, on the live range.
    range_ = Range::live;
    next_slow_.reset();
  }
  if (!dashboard_open) {
    // A dashboard opened later starts with every plan closed and no Space list loaded.
    open_plans_.clear();
    analysing_.clear();
    screens_.clear();
  }
  dashboard_open_ = dashboard_open;
  // A range takes effect even while the service is down.
  for (const Request& request : requests) {
    if (const auto* chosen = std::get_if<SelectRange>(&request)) {
      range_ = chosen->range;
      next_range_.reset();
    }
  }
}

void Poller::fetch_rest(WallTime now, bool dashboard_open, PollResult& result) {
  result.badge = fetch(*link_, ipc::alerts_badge_method, Json::object(), &parse_alerts_badge);
  const bool badge_changed = result.badge && result.badge != badge_;
  if (result.badge) {
    badge_ = result.badge;
  }
  bool incidents_sent = false;
  if (incidents_ != nullptr) {
    incidents_sent = incidents_->forward(*link_) > 0;
    result.incidents = incidents_->counts();
  }
  const bool slow = !next_slow_ || now >= *next_slow_;
  if (slow) {
    next_slow_ = now + slow_interval;
    result.drives = fetch(*link_, ipc::disks_health_method, Json::object(), &parse_disks_health);
  }
  if (slow || badge_changed || result.alert_action) {
    result.alerts = fetch(*link_, ipc::alerts_list_method, alerts_list_params(now - recent_alerts),
                          &parse_alerts_list);
  }
  if (dashboard_open) {
    result.top_by_cpu =
        fetch(*link_, ipc::processes_top_method, processes_top_params(ProcessSort::cpu, top_trees),
              &parse_processes_top);
    result.top_by_memory =
        fetch(*link_, ipc::processes_top_method,
              processes_top_params(ProcessSort::memory, top_trees), &parse_processes_top);
    if (slow || incidents_sent) {
      result.freezes =
          fetch(*link_, ipc::input_summary_method, Json::object(), &parse_input_summary);
      result.elimination =
          fetch(*link_, ipc::input_pauses_method, Json::object(), &parse_input_pauses);
    }
    if (range_ != Range::live && (!next_range_ || now >= *next_range_)) {
      next_range_ = now + slow_interval;
      result.range = fetch_range(now);
    }
  }
}

void Poller::handle(const Request& request, PollResult& result) {
  if (const auto* snooze = std::get_if<SnoozeAlert>(&request)) {
    const Result<Json> reply =
        link_->call(ipc::alerts_snooze_method,
                    alerts_snooze_params(snooze->rule, snooze->entity, snooze->duration));
    result.alert_action = reply ? std::string() : "Snooze failed: " + reply.error().message;
  } else if (const auto* dismiss = std::get_if<DismissAlert>(&request)) {
    const Result<Json> reply = link_->call(ipc::alerts_dismiss_method,
                                           alerts_dismiss_params(dismiss->rule, dismiss->entity));
    result.alert_action = reply ? std::string() : "Dismiss failed: " + reply.error().message;
  } else if (const auto* explain = std::get_if<ExplainProcess>(&request)) {
    const Result<Json> reply =
        link_->call(ipc::processes_explain_method, processes_explain_params(explain->pid));
    result.explanation = reply ? parse_processes_explain(*reply)
                               : Result<Explanation>(tl::unexpected(reply.error()));
  }
}

bool Poller::handle_cleanup(const Request& request, PollResult& result) {
  // Sends one of the owner's decisions; its outcome is empty when it worked.
  const auto decide = [&](std::string_view what, std::string_view method, const Json& params) {
    const Result<Json> reply = link_->call(method, params);
    result.cleanup.action =
        reply ? std::string() : std::string(what) + " failed: " + reply.error().message;
    return true;
  };
  if (std::holds_alternative<ScanNow>(request)) {
    return decide("Scan", ipc::cleaning_scan_method, Json::object());
  }
  if (const auto* approve = std::get_if<ApprovePlan>(&request)) {
    return decide("Approve", ipc::plans_approve_method,
                  plans_approve_params(approve->id, approve->selection));
  }
  if (const auto* reject = std::get_if<RejectPlan>(&request)) {
    return decide("Reject", ipc::plans_reject_method, plan_id_params(reject->id));
  }
  if (const auto* undo = std::get_if<UndoPlan>(&request)) {
    return decide("Undo", ipc::plans_undo_method, plan_id_params(undo->id));
  }
  if (const auto* restore = std::get_if<RestoreQuarantine>(&request)) {
    return decide("Restore", ipc::quarantine_restore_method,
                  quarantine_restore_params(restore->plan, restore->item));
  }
  if (const auto* purge = std::get_if<PurgeQuarantine>(&request)) {
    return decide("Deleting the quarantine", ipc::quarantine_purge_method,
                  Json{{"plan", purge->plan}});
  }
  if (const auto* propose = std::get_if<ProposeCleanup>(&request)) {
    return decide("Proposing a cleanup", ipc::plans_propose_cleanup_method,
                  propose_cleanup_params(propose->candidates));
  }
  if (const auto* action = std::get_if<OwnerAction>(&request)) {
    return decide("Starting it", ipc::plans_owner_action_method,
                  owner_action_params(action->candidate));
  }
  if (const auto* pause = std::get_if<ProposePause>(&request)) {
    decide("Proposing the pause", ipc::plans_propose_pause_method,
           Json{{"suspect", pause->suspect}, {"hours", pause->hours}});
    result.elimination =
        fetch(*link_, ipc::input_pauses_method, Json::object(), &parse_input_pauses);
    return true;
  }
  if (std::holds_alternative<EndPause>(request)) {
    decide("Ending the pause", ipc::input_end_pause_method, Json::object());
    result.elimination =
        fetch(*link_, ipc::input_pauses_method, Json::object(), &parse_input_pauses);
    return true;
  }
  if (const auto* open = std::get_if<OpenPlan>(&request)) {
    open_plans_.insert(open->id);
    if (std::optional<PlanDetail> detail =
            fetch(*link_, ipc::plans_get_method, plan_id_params(open->id), &parse_plans_get)) {
      result.cleanup.details.push_back(std::move(*detail));
    }
  } else if (const auto* close = std::get_if<ClosePlan>(&request)) {
    open_plans_.erase(close->id);
  } else if (const auto* load = std::get_if<LoadSpace>(&request)) {
    load_space(*load, result);
  }
  return false;
}

void Poller::handle_governor(const Request& request, PollResult& result) {
  const auto send = [&](std::string_view what, std::string_view method, const Json& params) {
    const Result<Json> reply = link_->call(method, params);
    result.governor_action =
        reply ? std::string() : std::string(what) + " failed: " + reply.error().message;
  };
  if (const auto* choice = std::get_if<ChooseForTree>(&request)) {
    send("The choice", ipc::governor_choose_method,
         governor_choose_params(choice->root, choice->choice));
  } else if (const auto* pause = std::get_if<PauseGovernor>(&request)) {
    send(pause->minutes.count() > 0 ? "Pausing the governor" : "Ending the pause",
         ipc::governor_pause_method, governor_pause_params(pause->minutes));
  } else if (const auto* game_mode = std::get_if<SetGameMode>(&request)) {
    send("Setting game mode", ipc::governor_game_mode_method,
         governor_game_mode_params(game_mode->value));
  } else if (const auto* orphans = std::get_if<DecideOrphans>(&request)) {
    send(orphans->decision == "forget" ? "Forgetting the rule" : "Answering the loops",
         ipc::governor_orphans_method, governor_orphans_params(orphans->id, orphans->decision));
  }
}

void Poller::fetch_governor(PollResult& result) {
  if (user_state_) {
    if (const std::optional<UserStateReading> reading = user_state_()) {
      static_cast<void>(
          link_->call(ipc::governor_user_state_method, governor_user_state_params(*reading)));
    }
  }
  result.governor =
      fetch(*link_, ipc::governor_status_method, Json::object(), &parse_governor_status);
}

void Poller::report_windows(WallTime now) {
  if (!windows_ || (next_windows_ && now < *next_windows_)) {
    return;
  }
  next_windows_ = now + windows_interval;
  const std::optional<std::vector<std::uint32_t>> pids = windows_();
  if (!pids) {
    return;
  }
  // A record holds at most max_record_elements ids: as many calls as needed.
  for (std::size_t first = 0; first < pids->size(); first += ipc::max_record_elements) {
    const std::size_t last = std::min(pids->size(), first + ipc::max_record_elements);
    Json chunk = Json::array();
    for (std::size_t index = first; index < last; ++index) {
      chunk.push_back((*pids)[index]);
    }
    static_cast<void>(
        link_->call(ipc::security_windows_method, Json{{"windows", {{"pids", std::move(chunk)}}}}));
  }
}

bool Poller::handle_screens(const Request& request, PollResult& result) {
  if (const auto* load = std::get_if<LoadScreen>(&request)) {
    screens_.load(*link_, *load, result.screens);
  } else if (const auto* action = std::get_if<ScreenAction>(&request)) {
    send_action(*link_, *action, result.screens);
    return true;
  }
  return false;
}

void Poller::ask_again(bool dashboard_open, const std::vector<Request>& requests,
                       PollResult& result) {
  if (!dashboard_open) {
    return;
  }
  std::vector<LoadScreen> loading;
  for (const Request& request : requests) {
    if (const auto* load = std::get_if<LoadScreen>(&request)) {
      loading.push_back(*load);
    }
  }
  screens_.ask_again(*link_, loading, result.screens);
  for (const LoadSpace& load : std::exchange(analysing_, {})) {
    const bool asked = std::ranges::any_of(requests, [&](const Request& request) {
      const auto* other = std::get_if<LoadSpace>(&request);
      return other != nullptr && other->list == load.list;
    });
    if (!asked) {
      load_space(load, result);
    }
  }
}

void Poller::load_space(const LoadSpace& load, PollResult& result) {
  SpaceReply space{.list = load.list, .volume = load.volume};
  const Result<Json> reply =
      link_->call(space_method(load.list), space_params(load.list, load.volume, load.refresh));
  const Result<void> parsed =
      reply ? parse_space(*reply, space) : Result<void>(tl::unexpected(reply.error()));
  if (!parsed) {
    space.state = "failed";
    space.error = parsed.error().message;
  }
  if (space.state == "scanning") {
    // Asked again, without starting another analysis, until it is ready.
    analysing_.push_back(LoadSpace{.list = load.list, .volume = load.volume, .refresh = false});
  }
  result.cleanup.space.push_back(std::move(space));
}

void Poller::fetch_cleanup(WallTime now, bool dashboard_open, bool refresh, PollResult& result) {
  bool due = refresh || !next_plans_ || now >= *next_plans_;
  if (dashboard_open || due || (status_ && status_->scanning)) {
    result.cleanup.status =
        fetch(*link_, ipc::cleaning_status_method, Json::object(), &parse_cleaning_status);
    if (result.cleanup.status) {
      // A scan that ended may have proposed plans.
      due = due || (status_ && status_->last_completed != result.cleanup.status->last_completed);
      status_ = result.cleanup.status;
    }
  }
  if (!due) {
    return;
  }
  next_plans_ = now + plans_interval;
  result.cleanup.plans = fetch(*link_, ipc::plans_list_method, Json::object(), &parse_plans_list);
  if (!dashboard_open) {
    return;
  }
  result.cleanup.quarantine =
      fetch(*link_, ipc::quarantine_list_method, Json::object(), &parse_quarantine_list);
  for (const std::int64_t id : open_plans_) {
    if (std::optional<PlanDetail> detail =
            fetch(*link_, ipc::plans_get_method, plan_id_params(id), &parse_plans_get)) {
      result.cleanup.details.push_back(std::move(*detail));
    }
  }
}

void Poller::run_tasks(PollResult& result) {
  if (tasks_ == nullptr) {
    return;
  }
  if (const std::optional<ITaskRunner::Report> report = tasks_->report()) {
    const Result<Json> reply = link_->call(ipc::user_tasks_report_method,
                                           Json{{"id", report->plan}, {"result", report->result}});
    if (!link_->reachable()) {
      return;  // delivered at a later poll; no new task meanwhile
    }
    // A refusal the service answered does not change when the same report is sent again.
    result.cleanup.task_note = reply ? report->summary
                                     : "The service refused the report of plan " +
                                           std::to_string(report->plan) + ": " +
                                           reply.error().message;
    tasks_->delivered();
  }
  if (tasks_->idle()) {
    const Result<Json> next = link_->call(ipc::user_tasks_next_method, Json::object());
    const auto task = next ? next->find("task") : Json::const_iterator{};
    if (next && link_->reachable() && task != next->end() && task->is_object()) {
      tasks_->start(*task);
    }
  }
  result.cleanup.task_activity = tasks_->activity();
}

std::optional<Series> Poller::query(const SeriesKey& key, std::int64_t from, std::int64_t to,
                                    std::optional<int> max_points) {
  const Result<Json> reply =
      link_->call(ipc::metrics_query_method,
                  metrics_query_params(key.metric, key.entity, from, to, max_points));
  if (!reply) {
    return std::nullopt;
  }
  Result<Series> series = parse_metrics_query(*reply);
  if (!series) {
    return std::nullopt;
  }
  // Under the key the tray knows the series by; the service answers with the series entity,
  // which differs from the token form metrics.latest gives the logical processors.
  series->key = key;
  return std::move(*series);
}

void Poller::fill_gaps(const LatestReply& latest, bool dashboard_open, PollResult& result) {
  for (const auto& [key, sample] : latest.metrics.values()) {
    const auto [previous, inserted] = newest_.try_emplace(key, sample.time);
    if (inserted || sample.time <= previous->second) {
      continue;
    }
    const WallTime last = std::exchange(previous->second, sample.time);
    const auto gap = sample.time - last;
    if (!dashboard_open || gap <= std::chrono::seconds{1} || gap > window_) {
      continue;
    }
    if (std::optional<Series> missed =
            query(key, unix_seconds(last) + 1, unix_seconds(sample.time), std::nullopt)) {
      result.history.push_back(std::move(*missed));
    }
  }
}

RangeHistory Poller::fetch_range(WallTime now) {
  RangeHistory history{.range = range_, .time = now, .series = {}};
  const std::int64_t to = unix_seconds(now) + 1;
  const std::int64_t from = unix_seconds(now) - range_length(range_, window_).count();
  for (const auto& entry : newest_) {
    const SeriesKey& key = entry.first;
    if (!charted(key)) {
      continue;
    }
    if (std::optional<Series> series = query(key, from, to, range_points)) {
      history.series.push_back(std::move(*series));
    }
  }
  return history;
}

}  // namespace myclean::tray_model
