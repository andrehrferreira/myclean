#include "core/governing/methods.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/games/fullscreen.hpp"
#include "core/governing/governor.hpp"
#include "core/governing/keys.hpp"
#include "core/governing/records.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/game_mode.hpp"
#include "core/governor/ladder.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/ipc/governor.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/rules/schema.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::governing {

namespace {

using Json = nlohmann::json;
using ipc::Param;
using ipc::ParamType;

std::int64_t integer(const Json& params, const char* name, std::int64_t fallback) {
  return params.contains(name) ? params.at(name).get<std::int64_t>() : fallback;
}

Result<Json> events(store::Store& store, const Json& params) {
  const std::int64_t since = integer(params, "since", 0);
  const std::int64_t limit = integer(params, "limit", ipc::default_events_limit);
  if (limit < 1 || limit > ipc::max_events_limit) {
    return fail(Errc::invalid_argument, "limit must be 1-" + std::to_string(ipc::max_events_limit));
  }
  return load_events(store, store::from_unix_seconds(since), static_cast<std::size_t>(limit))
      .transform([](const std::vector<GovernorEvent>& stored) {
        Json list = Json::array();
        for (const GovernorEvent& event : stored) {
          list.push_back(event_json(event));
        }
        return Json{{"events", std::move(list)}};
      });
}

Result<Json> choose(Governor& governor, const Json& params) {
  const std::int64_t pid = params.at("pid").get<std::int64_t>();
  const std::optional<governor::OwnerChoice> choice =
      governor::parse_owner_choice(params.at("choice").get<std::string>());
  if (pid < 0 || pid > std::numeric_limits<std::uint32_t>::max()) {
    return fail(Errc::invalid_argument, "pid out of range");
  }
  if (!choice) {
    return fail(Errc::invalid_argument, "choice must be resume, kill, allow_once or always_allow");
  }
  const governor::ProcessKey root{
      .pid = static_cast<std::uint32_t>(pid),
      .created = from_nanoseconds(params.at("created").get<std::int64_t>())};
  return Json{{"commands", governor.choose(root, *choice)}};
}

Result<Json> pause(Governor& governor, const Json& params) {
  const std::int64_t minutes = params.at("minutes").get<std::int64_t>();
  if (minutes < 0 || minutes > ipc::max_pause_minutes) {
    return fail(Errc::invalid_argument,
                "minutes must be 0-" + std::to_string(ipc::max_pause_minutes));
  }
  const std::optional<WallTime> until = governor.pause(std::chrono::minutes{minutes});
  return Json{{"paused_until", until ? Json(store::to_unix_seconds(*until)) : Json(nullptr)}};
}

Result<Json> game_mode(Governor& governor, const Json& params) {
  const std::optional<governor::GameModeOverride> value =
      governor::parse_game_mode_override(params.at("override").get<std::string>());
  if (!value) {
    return fail(Errc::invalid_argument, "override must be auto, on or off");
  }
  const governor::GameMode mode = governor.set_override(*value);
  return Json{{"active", mode.active}, {"build_cpu_percent", mode.build_cpu_percent}};
}

Result<Json> user_state(Governor& governor, const Json& params) {
  const std::optional<games::NotificationState> state =
      games::parse_notification_state(params.at("state").get<std::string>());
  const std::int64_t pid = params.at("foreground_pid").get<std::int64_t>();
  if (!state) {
    return fail(Errc::invalid_argument, "unknown notification state");
  }
  if (pid < 0 || pid > std::numeric_limits<std::uint32_t>::max()) {
    return fail(Errc::invalid_argument, "foreground_pid out of range");
  }
  governor.set_user_state(
      UserState{.state = *state, .foreground_pid = static_cast<std::uint32_t>(pid)});
  return Json::object();
}

}  // namespace

Json process_json(const governor::ProcessKey& key) {
  return Json{{"pid", key.pid}, {"created", to_nanoseconds(key.created)}};
}

Json metrics_json(const std::optional<governor::PressureSample>& metrics) {
  if (!metrics) {
    return nullptr;
  }
  return Json{{"cpu_percent", metrics->cpu_percent},
              {"available", metrics->available.count()},
              {"commit", metrics->commit.count()},
              {"commit_limit", metrics->commit_limit.count()},
              {"hard_faults_per_second", metrics->hard_faults_per_second}};
}

Json event_json(const GovernorEvent& event) {
  return Json{
      {"id", event.id},
      {"time", store::to_unix_seconds(event.time)},
      {"kind", to_string(event.kind)},
      {"root", event.root ? process_json(*event.root) : Json(nullptr)},
      {"image", event.root_image},
      {"class", event.process_class ? Json(rules::to_string(*event.process_class)) : Json(nullptr)},
      {"step", event.step ? Json(governor::to_string(*event.step)) : Json(nullptr)},
      {"reason", governor::to_string(event.reason)},
      {"actor", event.actor.to_string()},
      {"members", event.members},
      {"metrics", metrics_json(event.metrics)},
      {"detail", event.detail}};
}

Result<void> register_methods(ipc::Dispatcher& methods, Governor& governor, store::Store& store) {
  const auto add = [&methods](std::string_view name, std::vector<Param> params,
                              ipc::Handler handler, ipc::Access access) {
    return methods.add(std::string(name), std::move(params), std::move(handler), access);
  };
  return add(
             ipc::governor_status_method, {},
             [&governor](const Json&) -> Result<Json> { return governor.status(); },
             ipc::Access::any)
      .and_then([&] {
        return add(
            ipc::governor_events_method,
            {Param{.name = "since", .type = ParamType::integer},
             Param{.name = "limit", .type = ParamType::integer}},
            [&store](const Json& params) { return events(store, params); }, ipc::Access::any);
      })
      .and_then([&] {
        return add(
            ipc::governor_choose_method,
            {Param{.name = "pid", .type = ParamType::integer, .required = true},
             Param{.name = "created", .type = ParamType::integer, .required = true},
             Param{.name = "choice", .type = ParamType::token, .required = true}},
            [&governor](const Json& params) { return choose(governor, params); },
            ipc::Access::tray);
      })
      .and_then([&] {
        return add(
            ipc::governor_pause_method,
            {Param{.name = "minutes", .type = ParamType::integer, .required = true}},
            [&governor](const Json& params) { return pause(governor, params); }, ipc::Access::tray);
      })
      .and_then([&] {
        return add(
            ipc::governor_game_mode_method,
            {Param{.name = "override", .type = ParamType::token, .required = true}},
            [&governor](const Json& params) { return game_mode(governor, params); },
            ipc::Access::tray);
      })
      .and_then([&] {
        return add(
            ipc::governor_user_state_method,
            {Param{.name = "state", .type = ParamType::token, .required = true},
             Param{.name = "foreground_pid", .type = ParamType::integer, .required = true}},
            [&governor](const Json& params) { return user_state(governor, params); },
            ipc::Access::tray);
      });
}

}  // namespace myclean::governing
