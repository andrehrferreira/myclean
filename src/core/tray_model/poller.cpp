#include "core/tray_model/poller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/alerts.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/input.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/replies.hpp"
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
    badge_.reset();
  }
  if (reply) {
    if (Result<LatestReply> latest = parse_metrics_latest(*reply)) {
      fill_gaps(*latest, dashboard_open, result);
      result.latest = std::move(*latest);
    }
  }
  for (const Request& request : requests) {
    handle(request, result);
  }
  fetch_rest(now, dashboard_open, result);
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
