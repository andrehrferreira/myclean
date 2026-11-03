#include "core/tray_model/governor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::tray_model {

namespace {

using Json = nlohmann::json;

WallTime seconds_time(const Json& value) {
  return WallTime{std::chrono::seconds{value.get<std::int64_t>()}};
}

GovernorProcess process(const Json& value) {
  return GovernorProcess{.pid = value.at("pid").get<std::uint32_t>(),
                         .created = value.at("created").get<std::int64_t>()};
}

std::vector<std::string> strings(const Json& array) {
  std::vector<std::string> values;
  for (const Json& value : array) {
    values.push_back(value.get<std::string>());
  }
  return values;
}

GovernorEventRow event_row(const Json& event) {
  const Json& step = event.at("step");
  return GovernorEventRow{
      .time = seconds_time(event.at("time")),
      .kind = event.at("kind").get<std::string>(),
      .image = event.at("image").get<std::string>(),
      .step = step.is_null() ? std::nullopt : std::optional(step.get<std::string>()),
      .reason = event.at("reason").get<std::string>(),
      .actor = event.at("actor").get<std::string>(),
      .detail = event.at("detail").get<std::string>()};
}

void read_game_mode(const Json& game_mode, GovernorStatus& status) {
  status.game_mode = game_mode.at("active").get<bool>();
  status.game_override = game_mode.at("override").get<std::string>();
  status.build_cpu_percent = game_mode.at("build_cpu_percent").get<int>();
  for (const Json& game : game_mode.at("games")) {
    status.games.push_back(GovernorGame{.image = game.at("image").get<std::string>(),
                                        .reason = game.at("reason").get<std::string>()});
  }
}

// "the CPU was busy" and the like, for a toast.
std::string_view reason_text(std::string_view reason) {
  if (reason == "cpu") {
    return "the CPU stayed busy";
  }
  if (reason == "available_memory") {
    return "memory ran low";
  }
  if (reason == "commit") {
    return "committed memory neared its limit";
  }
  if (reason == "hard_faults") {
    return "the disk was paging hard";
  }
  return reason;
}

}  // namespace

Result<GovernorStatus> parse_governor_status(const Json& reply) {
  try {
    GovernorStatus status;
    status.mode = reply.at("mode").get<std::string>();
    if (const Json& until = reply.at("paused_until"); !until.is_null()) {
      status.paused_until = seconds_time(until);
    }
    const Json& pressure = reply.at("pressure");
    status.pressure = pressure.at("state").get<std::string>() == "pressure";
    status.clearing = pressure.at("clearing").get<bool>();
    status.reasons = strings(pressure.at("reasons"));
    read_game_mode(reply.at("game_mode"), status);
    for (const Json& tree : reply.at("trees")) {
      status.trees.push_back(GovernorTree{.root = process(tree.at("root")),
                                          .image = tree.at("image").get<std::string>(),
                                          .process_class = tree.at("class").get<std::string>(),
                                          .step = tree.at("step").get<std::string>()});
    }
    for (const Json& notification : reply.at("notifications")) {
      status.notifications.push_back(
          GovernorNotification{.id = notification.at("id").get<std::int64_t>(),
                               .time = seconds_time(notification.at("time")),
                               .root = process(notification.at("root")),
                               .image = notification.at("image").get<std::string>(),
                               .process_class = notification.at("class").get<std::string>(),
                               .step = notification.at("step").get<std::string>(),
                               .reason = notification.at("reason").get<std::string>(),
                               .members = notification.at("members").get<std::int64_t>()});
    }
    for (const Json& event : reply.at("events")) {
      status.events.push_back(event_row(event));
    }
    return status;
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string("governor.status: ") + error.what());
  }
}

Json governor_choose_params(const GovernorProcess& root, std::string_view choice) {
  return Json{{"pid", root.pid}, {"created", root.created}, {"choice", choice}};
}

Json governor_pause_params(std::chrono::minutes minutes) {
  return Json{{"minutes", minutes.count()}};
}

Json governor_game_mode_params(std::string_view value) {
  return Json{{"override", value}};
}

Json governor_user_state_params(const UserStateReading& reading) {
  return Json{{"state", reading.state}, {"foreground_pid", reading.foreground_pid}};
}

GovernorToast governor_toast(const GovernorNotification& notification) {
  std::string_view action = "slowed";
  if (notification.step == "cap_job") {
    action = "capped";
  } else if (notification.step == "suspend") {
    action = "suspended";
  } else if (notification.step == "kill") {
    action = "ended";
  }
  const bool severe = notification.step == "suspend" || notification.step == "kill";
  return GovernorToast{.title = "MyClean " + std::string(action) + " " + notification.image,
                       .detail = std::to_string(notification.members) +
                                 (notification.members == 1 ? " process: " : " processes: ") +
                                 std::string(reason_text(notification.reason)) +
                                 ". Click to Resume, Kill, Allow once or Always allow.",
                       .severe = severe};
}

std::vector<GovernorToast> GovernorToasts::take(const GovernorStatus& status) {
  std::vector<GovernorToast> toasts;
  for (const GovernorNotification& notification : status.notifications) {
    const std::pair<WallTime, std::int64_t> order{notification.time, notification.id};
    if (newest_ && order <= *newest_) {
      continue;
    }
    newest_ = order;
    if (!baseline_) {
      continue;
    }
    const bool always = notification.step == "suspend" || notification.step == "kill";
    const auto last = std::ranges::find(last_by_tree_, notification.root,
                                        &std::pair<GovernorProcess, WallTime>::first);
    if (last == last_by_tree_.end()) {
      last_by_tree_.emplace_back(notification.root, notification.time);
    } else if (always || notification.time - last->second >= interval_) {
      last->second = notification.time;
    } else {
      continue;
    }
    toasts.push_back(governor_toast(notification));
  }
  baseline_ = true;
  // Trees whose last toast is past the interval no longer limit anything.
  if (newest_) {
    const WallTime now = newest_->first;
    std::erase_if(last_by_tree_,
                  [&](const auto& entry) { return now - entry.second >= interval_; });
  }
  return toasts;
}

}  // namespace myclean::tray_model
