#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "core/actions/policy.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"

// The user-scope plans, which the tray runs with the owner's token: user_tasks.next hands them
// out, user_tasks.report records what the tray did (contracts in core/ipc/plans.hpp). The
// service never runs these actions itself.
namespace myclean::cleaning {

class TrayTasks {
 public:
  // Every reference must outlive the tasks. Thread-safe.
  TrayTasks(store::Store& store, actions::PolicyEngine& engine, const os::IClock& clock)
      : store_(&store), engine_(&engine), clock_(&clock) {}

  // The user_tasks.next answer: the oldest queued undo, else the oldest approved user-scope
  // plan, that was not handed out in the last ipc::task_claim_minutes.
  [[nodiscard]] Result<nlohmann::json> next();

  // Records the user_tasks.report `result` (already checked against its schema) for plan `id`
  // and returns the plan's new state.
  [[nodiscard]] Result<PlanState> report(PlanId id, const nlohmann::json& result);

  // Queues the undo of applied or verified user-scope plan `id` for the tray.
  [[nodiscard]] Result<PlanState> queue_undo(PlanId id);

 private:
  [[nodiscard]] Result<nlohmann::json> task(PlanId id, bool undo);

  store::Store* store_;
  actions::PolicyEngine* engine_;
  const os::IClock* clock_;
  std::mutex mutex_;
  std::map<std::int64_t, WallTime> claims_;  // plan -> when handed out
  std::set<std::int64_t> undos_;             // plans whose undo is queued
};

}  // namespace myclean::cleaning
