#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "core/governing/governor.hpp"
#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/pressure.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/store/store.hpp"

// The governor's IPC methods (contracts in core/ipc/governor.hpp) and the JSON they share.
namespace myclean::governing {

// {"pid", "created"}: created in nanoseconds since the Unix epoch.
[[nodiscard]] nlohmann::json process_json(const governor::ProcessKey& key);
// {"cpu_percent", "available", "commit", "commit_limit", "hard_faults_per_second"} or null.
[[nodiscard]] nlohmann::json metrics_json(const std::optional<governor::PressureSample>& metrics);
// One event as core/ipc/governor.hpp describes it.
[[nodiscard]] nlohmann::json event_json(const GovernorEvent& event);

// Registers governor.status and governor.events for every client, and governor.choose,
// governor.pause, governor.game_mode and governor.user_state for the owner's tray only
// (ipc::Access::tray). `governor` and `store` must outlive the dispatcher's use.
[[nodiscard]] Result<void> register_methods(ipc::Dispatcher& methods, Governor& governor,
                                            store::Store& store);

}  // namespace myclean::governing
