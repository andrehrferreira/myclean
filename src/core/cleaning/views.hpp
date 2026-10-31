#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/store.hpp"

// The read side of plans and quarantine, in the JSON forms of core/ipc/plans.hpp.
namespace myclean::cleaning {

// plans.list: the newest ipc::max_listed_plans plans (only those in `state` when given).
[[nodiscard]] Result<nlohmann::json> list_plans(store::Store& store,
                                                std::optional<PlanState> state);
// plans.get. Fails with not_found for an unknown plan.
[[nodiscard]] Result<nlohmann::json> get_plan(store::Store& store, PlanId id);
// quarantine.list: every run with held items.
[[nodiscard]] Result<nlohmann::json> list_quarantine(store::Store& store);

}  // namespace myclean::cleaning
