#include "core/tray_model/screens.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ipc/apps.hpp"
#include "core/ipc/audit.hpp"
#include "core/ipc/backup.hpp"
#include "core/ipc/plans.hpp"
#include "core/ipc/reports.hpp"
#include "core/ipc/resident.hpp"
#include "core/ipc/security.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/audit.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/inventory.hpp"
#include "core/tray_model/json_read.hpp"
#include "core/tray_model/optimizer.hpp"
#include "core/tray_model/reports.hpp"
#include "core/tray_model/service_link.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;

// The parameters of a space analysis: refresh only when asked for.
Json refresh_params(bool refresh) {
  return refresh ? Json{{"refresh", true}} : Json::object();
}

ScreenCall call_of(std::string_view method, Json params = Json::object()) {
  return ScreenCall{.method = std::string(method), .params = std::move(params)};
}

// The span of the Security screen's network talkers.
constexpr int network_hours = 24;

Json candidates(const std::vector<std::int64_t>& ids) {
  return Json{{"ids", ids}};
}

// What the owner proposed, in the failure text.
std::string_view action_name(ScreenAction::Kind kind) {
  switch (kind) {
    case ScreenAction::Kind::system_setting:
      return "Proposing the setting";
    case ScreenAction::Kind::relocation:
      return "Proposing the relocation";
    case ScreenAction::Kind::startup:
      return "Proposing the startup change";
    case ScreenAction::Kind::index_exclusion:
      return "Proposing the index exclusion";
    case ScreenAction::Kind::compact:
      return "Proposing the compaction";
    case ScreenAction::Kind::uninstall:
      return "Proposing the uninstall";
    case ScreenAction::Kind::defender_scan:
      break;
  }
  return "Starting the Defender scan";
}

ActionOutcome failure(ScreenAction::Kind kind, const Error& error) {
  return ActionOutcome{.text = std::format("{} failed: {}", action_name(kind), error.message),
                       .failed = true};
}

// An answer whose analysis still runs.
bool running(const Json& reply) {
  const auto state = reply.find("state");
  return state != reply.end() && state->is_string() &&
         (*state == "scanning" || *state == "checking");
}

// What names an item of a list answer across answers.
std::string listed_key(ScreenAction::Kind kind, const Json& item) {
  if (kind == ScreenAction::Kind::uninstall) {
    return app_key(item);
  }
  if (kind == ScreenAction::Kind::defender_scan) {
    return persistence_key(item);
  }
  return item.at("key").get<std::string>();
}

// What the owner's proposal led to.
std::string outcome_text(const ScreenAction& action, const Json& reply) {
  if (action.kind == ScreenAction::Kind::defender_scan) {
    return std::format("Microsoft Defender scans {}; its verdict shows after the next Refresh.",
                       json_read::text(reply, "image"));
  }
  const std::optional<std::int64_t> plan = json_read::optional_int(reply, "plan");
  std::string text = plan ? std::format("Plan {} waits for your approval in Cleanup.", *plan)
                          : std::string("Proposed; it waits for your approval in Cleanup.");
  if (json_read::text(reply, "verdict") == "keep") {
    text += std::format(" MyClean's verdict is keep: {}.", json_read::text(reply, "reason"));
  }
  return text;
}

template <typename T>
void store(Answer<T>& slot, const Result<Json>& reply, Result<T> (*parse)(const Json&)) {
  slot = reply ? parse(*reply) : Result<T>(tl::unexpected(reply.error()));
}

}  // namespace

std::vector<ScreenCall> screen_calls(const LoadScreen& load) {
  const bool refresh = load.refresh;
  switch (load.screen) {
    case Screen::placement:
      return {call_of(ipc::system_space_status_method)};
    case Screen::optimize:
      return {call_of(ipc::space_indexed_folders_method, refresh_params(refresh)),
              call_of(ipc::space_vhdx_method, refresh_params(refresh)),
              call_of(ipc::space_dev_drive_method, refresh_params(refresh)),
              call_of(ipc::resident_costs_method)};
    case Screen::startup:
      return {call_of(ipc::startup_list_method), call_of(ipc::startup_boot_times_method)};
    case Screen::apps:
      return {call_of(ipc::apps_list_method)};
    case Screen::security:
      return {call_of(ipc::security_scan_method), call_of(ipc::security_extensions_method),
              call_of(ipc::security_network_method, Json{{"hours", network_hours}})};
    case Screen::backups:
      return {call_of(ipc::backups_check_method, refresh_params(refresh))};
    case Screen::audit:
      return {call_of(ipc::audit_list_method, audit_list_params(load.before))};
    case Screen::settings:
      // VirusTotal's state comes with the scan, the workspace roots with the backup check.
      return {call_of(ipc::security_scan_method), call_of(ipc::backups_check_method)};
    case Screen::reports:
      break;
  }
  if (load.month.empty()) {
    return {call_of(ipc::reports_list_method)};
  }
  if (load.other.empty()) {
    return {call_of(ipc::reports_get_method, Json{{"month", load.month}})};
  }
  return {call_of(ipc::reports_compare_method, Json{{"a", load.month}, {"b", load.other}})};
}

std::optional<ScreenCall> list_call(const ScreenAction& action) {
  switch (action.kind) {
    case ScreenAction::Kind::startup:
      return call_of(ipc::startup_list_method);
    case ScreenAction::Kind::uninstall:
      return call_of(ipc::apps_list_method);
    case ScreenAction::Kind::defender_scan:
      return call_of(ipc::security_scan_method);
    default:
      return std::nullopt;
  }
}

Result<std::vector<std::int64_t>> fresh_ids(const ScreenAction& action, const Json& list) {
  try {
    std::vector<std::int64_t> ids;
    const Json& items = list.at(action.kind == ScreenAction::Kind::uninstall ? "apps" : "items");
    for (const std::string& key : action.keys) {
      const auto found = std::ranges::find_if(
          items, [&](const Json& item) { return listed_key(action.kind, item) == key; });
      if (found == items.end()) {
        return fail(Errc::not_found, key + " is no longer listed");
      }
      ids.push_back(found->at("id").get<std::int64_t>());
    }
    return ids;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("the list answer: ") + error.what());
  }
}

ScreenCall action_call(const ScreenAction& action, const std::vector<std::int64_t>& ids) {
  switch (action.kind) {
    case ScreenAction::Kind::system_setting:
      return call_of(ipc::plans_propose_optimization_method, Json{{"kind", action.value}});
    case ScreenAction::Kind::relocation:
      return call_of(ipc::plans_propose_optimization_method,
                     Json{{"kind", "relocate"}, {"cache", action.value}});
    case ScreenAction::Kind::startup:
      return call_of(ipc::plans_propose_startup_method,
                     Json{{"candidates", candidates(ids)}, {"enable", action.enable}});
    case ScreenAction::Kind::index_exclusion:
      return call_of(ipc::plans_propose_index_exclusion_method,
                     Json{{"candidates", candidates(ids)}});
    case ScreenAction::Kind::compact:
      return call_of(ipc::plans_propose_compact_method, Json{{"candidates", candidates(ids)}});
    case ScreenAction::Kind::uninstall:
      return call_of(ipc::plans_propose_uninstall_method, Json{{"app", ids.at(0)}});
    case ScreenAction::Kind::defender_scan:
      break;
  }
  return call_of(ipc::security_defender_scan_method, Json{{"item", ids.at(0)}});
}

void ScreenFetcher::call(IServiceLink& link, const ScreenCall& call, ScreensPoll& poll) {
  Result<Json> reply = link.call(call.method, call.params);
  if (reply && running(*reply)) {
    // Asked again without starting another analysis, until it is ready.
    Json params = call.params;
    params.erase("refresh");
    ScreenCall again{.method = call.method, .params = std::move(params)};
    if (std::ranges::find(running_, again) == running_.end()) {
      running_.push_back(std::move(again));
    }
  }
  poll.answers.push_back(ScreenAnswer{.method = call.method, .reply = std::move(reply)});
}

void ScreenFetcher::load(IServiceLink& link, const LoadScreen& load, ScreensPoll& poll) {
  for (const ScreenCall& screen_call : screen_calls(load)) {
    call(link, screen_call, poll);
  }
}

void send_action(IServiceLink& link, const ScreenAction& action, ScreensPoll& poll) {
  std::vector<std::int64_t> ids = action.ids;
  if (const std::optional<ScreenCall> list = list_call(action)) {
    Result<Json> listed = link.call(list->method, list->params);
    Result<std::vector<std::int64_t>> fresh =
        listed ? fresh_ids(action, *listed)
               : Result<std::vector<std::int64_t>>(tl::unexpected(listed.error()));
    poll.answers.push_back(ScreenAnswer{.method = list->method, .reply = std::move(listed)});
    if (!fresh) {
      poll.action = failure(action.kind, fresh.error());
      return;
    }
    ids = *std::move(fresh);
  }
  const ScreenCall proposal = action_call(action, ids);
  const Result<Json> reply = link.call(proposal.method, proposal.params);
  if (!reply) {
    poll.action = failure(action.kind, reply.error());
    return;
  }
  poll.action = ActionOutcome{.text = outcome_text(action, *reply), .failed = false};
}

void ScreenFetcher::ask_again(IServiceLink& link, const std::vector<LoadScreen>& loading,
                              ScreensPoll& poll) {
  for (const ScreenCall& again : std::exchange(running_, {})) {
    const bool loaded = std::ranges::any_of(loading, [&](const LoadScreen& load) {
      return std::ranges::any_of(screen_calls(load), [&](const ScreenCall& other) {
        return other.method == again.method;
      });
    });
    if (!loaded) {
      call(link, again, poll);
    }
  }
}

void ScreensView::apply(ScreensPoll poll) {
  for (const ScreenAnswer& answer : poll.answers) {
    const std::string_view method = answer.method;
    if (method == ipc::system_space_status_method) {
      store(system_space_, answer.reply, &parse_system_space);
    } else if (method == ipc::space_indexed_folders_method) {
      store(indexed_, answer.reply, &parse_indexed_folders);
    } else if (method == ipc::space_vhdx_method) {
      store(vhdx_, answer.reply, &parse_vhdx);
    } else if (method == ipc::space_dev_drive_method) {
      store(dev_drive_, answer.reply, &parse_dev_drive);
    } else if (method == ipc::resident_costs_method) {
      store(costs_, answer.reply, &parse_resident_costs);
    } else if (method == ipc::startup_list_method) {
      store(startup_, answer.reply, &parse_startup_list);
    } else if (method == ipc::startup_boot_times_method) {
      store(boot_times_, answer.reply, &parse_boot_times);
    } else if (method == ipc::apps_list_method) {
      store(apps_, answer.reply, &parse_apps_list);
    } else if (method == ipc::security_scan_method) {
      store(scan_, answer.reply, &parse_security_scan);
    } else if (method == ipc::security_extensions_method) {
      store(extensions_, answer.reply, &parse_security_extensions);
    } else if (method == ipc::security_network_method) {
      store(network_, answer.reply, &parse_security_network);
    } else if (method == ipc::backups_check_method) {
      store(backups_, answer.reply, &parse_backups_check);
    } else if (method == ipc::audit_list_method) {
      store(audit_, answer.reply, &parse_audit_list);
    } else if (method == ipc::reports_list_method) {
      store(reports_, answer.reply, &parse_reports_list);
    } else if (method == ipc::reports_get_method) {
      store(report_, answer.reply, &parse_reports_get);
    } else if (method == ipc::reports_compare_method) {
      store(report_, answer.reply, &parse_reports_compare);
    }
  }
  if (poll.action) {
    action_ = std::move(poll.action);
  }
}

std::string answer_error(const Error& error) {
  return std::format("{} ({})", not_available, error.message);
}

}  // namespace myclean::tray_model
