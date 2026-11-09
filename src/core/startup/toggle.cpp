#include "core/startup/toggle.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/startup/startup.hpp"

namespace myclean::startup {
namespace {

using Json = nlohmann::json;

// A plan item's path is the item id, as UTF-8 text: names are byte-exact, backslashes included.
std::filesystem::path id_path(const std::string& id) {
  return {std::u8string(id.begin(), id.end())};
}

std::string id_of(const PlanItem& item) {
  return utf8_text(item.path);
}

// One item's state as far as the toggle is concerned.
struct State {
  ApprovedValue approved;             // run, run32, folder
  std::optional<bool> task_enabled;   // task
  std::optional<ServiceStart> start;  // service

  friend bool operator==(const State&, const State&) = default;
};

ApprovedKey approved_key_of(Source source) {
  if (source == Source::run32) {
    return ApprovedKey::run32;
  }
  return source == Source::startup_folder ? ApprovedKey::startup_folder : ApprovedKey::run;
}

State state_of(const Item& item) {
  switch (item.source) {
    case Source::task:
      return State{.task_enabled = item.enabled.value_or(true)};
    case Source::service:
      return State{.start = item.start};
    default:
      return State{.approved = item.approved};
  }
}

bool in_requested_state(const Item& item, bool enable) {
  return item.enabled == enable;
}

// The state that disables or enables `item`, or nullopt when it is in it already.
std::optional<State> target_state(const Item& item, bool enable, WallTime now) {
  if (in_requested_state(item, enable)) {
    return std::nullopt;
  }
  switch (item.source) {
    case Source::task:
      return State{.task_enabled = enable};
    case Source::service:
      return State{.start = ServiceStart{.type = StartType::manual, .delayed = false}};
    default:
      return State{.approved = approved_bytes(!enable, now)};
  }
}

Result<void> write_state(IStartupSystem& system, const ItemKey& key, const State& state) {
  switch (key.source) {
    case Source::task:
      return system.set_task_enabled(key.name, state.task_enabled.value_or(true));
    case Source::service:
      if (!state.start) {
        return fail(Errc::invalid_data, "no start type recorded for service " + key.name);
      }
      return system.set_service_start(key.name, *state.start);
    default:
      return system.set_approved(key.scope, approved_key_of(key.source), key.name, state.approved);
  }
}

Json state_json(const State& state) {
  Json json = Json::object();
  if (state.task_enabled) {
    json["task_enabled"] = *state.task_enabled;
  } else if (state.start) {
    json["start"] = to_string(state.start->type);
    json["delayed"] = state.start->delayed;
  } else {
    json["approved"] = state.approved ? Json(*state.approved) : Json(nullptr);
  }
  return json;
}

Result<State> parse_state(const Json& json) {
  if (json.contains("task_enabled")) {
    return State{.task_enabled = json.at("task_enabled").get<bool>()};
  }
  if (json.contains("start")) {
    const std::optional<StartType> type = parse_start_type(json.at("start").get<std::string>());
    if (!type) {
      return fail(Errc::invalid_data, "unknown start type in the undo data");
    }
    return State{.start = ServiceStart{.type = *type, .delayed = json.at("delayed").get<bool>()}};
  }
  const Json& approved = json.at("approved");
  return State{.approved = approved.is_null()
                               ? ApprovedValue{}
                               : ApprovedValue{approved.get<std::vector<std::uint8_t>>()}};
}

struct Change {
  ItemKey key;
  std::string id;
  State before;
  State written;
};

Result<std::vector<Change>> parse_undo(const Undo& undo) {
  std::vector<Change> changes;
  try {
    for (const UndoItem& item : undo.items) {
      const Json json = Json::parse(item.data);
      const std::string id = json.at("id").get<std::string>();
      Result<ItemKey> key = parse_item_id(id);
      Result<State> before = parse_state(json.at("before"));
      Result<State> written = parse_state(json.at("written"));
      if (!key) {
        return tl::unexpected(key.error());
      }
      if (!before) {
        return tl::unexpected(before.error());
      }
      if (!written) {
        return tl::unexpected(written.error());
      }
      changes.push_back(Change{.key = *std::move(key),
                               .id = id,
                               .before = *std::move(before),
                               .written = *std::move(written)});
    }
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("startup undo data: ") + error.what());
  }
  return changes;
}

// The state of the changed item now; nullopt when it is gone. A service set to Manual is no
// longer a startup item: its start type comes from the snapshot's service list.
std::optional<State> current_state(const Snapshot& snapshot, const std::vector<Item>& items,
                                   const Change& change) {
  if (change.key.source == Source::service) {
    const auto service = std::ranges::find(snapshot.services, change.key.name, &Service::name);
    return service == snapshot.services.end() ? std::nullopt
                                              : std::optional{State{.start = service->start}};
  }
  const Item* item = find_item(items, change.id);
  return item == nullptr ? std::nullopt : std::optional{state_of(*item)};
}

// Whether `now` is the state apply wrote. A written StartupApproved value is compared by its
// flag only: Windows may rewrite the time.
bool still_written(const State& now, const State& written) {
  if (written.task_enabled || written.start) {
    return now == written;
  }
  return approved_disabled(now.approved) == approved_disabled(written.approved);
}

Result<std::vector<Item>> read_inventory(const IStartupSystem& system) {
  Result<Snapshot> snapshot = system.read();
  if (!snapshot) {
    return tl::unexpected(snapshot.error());
  }
  return inventory(*snapshot);
}

}  // namespace

ActionInfo StartupToggle::describe() const {
  return ActionInfo{
      .what = enable_ ? "Enable startup items" : "Disable startup items",
      .why =
          "Programs that start with Windows slow the logon and run in the background; the "
          "Run value, the shortcut, the task or the service stays, so undo brings each back",
      .risk = Risk::low,
      .tier = Tier::ask,
      .context = ExecutionContext::service};
}

Result<Preview> StartupToggle::preview() {
  Result<std::vector<Item>> items = read_inventory(*system_);
  if (!items) {
    return tl::unexpected(items.error());
  }
  Preview preview;
  for (const std::string& id : ids_) {
    const Item* item = find_item(*items, id);
    if (item == nullptr) {
      return fail(Errc::not_found, "no startup item " + id);
    }
    if (!toggleable(*item)) {
      return fail(Errc::invalid_argument, "a RunOnce item cannot be disabled: " + id);
    }
    if (enable_ && item->source == Source::service) {
      return fail(Errc::invalid_argument,
                  "a service is enabled again only by undoing its disable: " + id);
    }
    if (std::ranges::count(ids_, id) > 1) {
      return fail(Errc::invalid_argument, "startup item listed twice: " + id);
    }
    preview.items.push_back(PlanItem{.path = id_path(id)});
  }
  return preview;
}

Result<Undo> StartupToggle::apply(const Preview& preview) {
  Result<std::vector<Item>> items = read_inventory(*system_);
  if (!items) {
    return tl::unexpected(items.error());
  }
  const WallTime now = clock_->now();
  Undo undo;
  std::vector<Change> done;
  const auto reverse = [&] {
    for (const Change& change : std::views::reverse(done)) {
      static_cast<void>(write_state(*system_, change.key, change.before));
    }
  };
  for (const PlanItem& planned : preview.items) {
    const std::string id = id_of(planned);
    const Item* item = find_item(*items, id);
    if (item == nullptr || !toggleable(*item) || (enable_ && item->source == Source::service)) {
      reverse();
      return fail(item == nullptr ? Errc::not_found : Errc::invalid_argument,
                  "cannot change startup item " + id);
    }
    const std::optional<State> written = target_state(*item, enable_, now);
    if (!written) {
      continue;
    }
    Result<ItemKey> key = parse_item_id(id);
    if (!key) {
      reverse();
      return tl::unexpected(key.error());
    }
    Change change{.key = *std::move(key), .id = id, .before = state_of(*item), .written = *written};
    if (Result<void> wrote = write_state(*system_, change.key, change.written); !wrote) {
      reverse();
      return tl::unexpected(wrote.error());
    }
    undo.items.push_back(UndoItem{.item = planned.id,
                                  .data = Json{{"id", id},
                                               {"before", state_json(change.before)},
                                               {"written", state_json(change.written)}}
                                              .dump()});
    done.push_back(std::move(change));
  }
  if (undo.items.empty()) {
    return fail(Errc::invalid_state, enable_ ? "every startup item is enabled already"
                                             : "every startup item is disabled already");
  }
  return undo;
}

Result<Verification> StartupToggle::verify(const Undo& undo) {
  Result<std::vector<Change>> changes = parse_undo(undo);
  if (!changes) {
    return tl::unexpected(changes.error());
  }
  Result<Snapshot> snapshot = system_->read();
  if (!snapshot) {
    return tl::unexpected(snapshot.error());
  }
  const std::vector<Item> items = inventory(*snapshot);
  const auto applied = std::ranges::count_if(*changes, [&](const Change& change) {
    const std::optional<State> now = current_state(*snapshot, items, change);
    return now && still_written(*now, change.written);
  });
  return Verification{.metric = "startup_items_changed",
                      .before = 0.0,
                      .after = static_cast<double>(applied),
                      .effect = applied == std::ssize(*changes) ? Effect::better : Effect::worse};
}

Result<void> StartupToggle::undo(const Undo& undo) {
  Result<std::vector<Change>> changes = parse_undo(undo);
  if (!changes) {
    return tl::unexpected(changes.error());
  }
  Result<Snapshot> snapshot = system_->read();
  if (!snapshot) {
    return tl::unexpected(snapshot.error());
  }
  const std::vector<Item> items = inventory(*snapshot);
  for (const Change& change : *changes) {
    const std::optional<State> now = current_state(*snapshot, items, change);
    if (!now) {
      return fail(Errc::invalid_state, "startup item " + change.id + " is gone");
    }
    if (!still_written(*now, change.written)) {
      return fail(Errc::invalid_state, "startup item " + change.id + " was changed meanwhile");
    }
  }
  for (const Change& change : *changes) {
    if (Result<void> wrote = write_state(*system_, change.key, change.before); !wrote) {
      return tl::unexpected(wrote.error());
    }
  }
  return {};
}

}  // namespace myclean::startup
