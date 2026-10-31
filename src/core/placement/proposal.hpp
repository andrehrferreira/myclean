#pragma once

#include <string>

#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::placement {

// A plan named `action_name` ("RelocateCache", "SetHibernation", ...) over the items `action`
// previews now.
[[nodiscard]] Result<Plan> plan_of(Action& action, std::string action_name, const Actor& proposer,
                                   WallTime now);

// Submits a placement plan. Relocations and system space settings change only after the owner
// approves them: a plan proposed at level `automatic` waits in awaiting_approval like one at
// `suggest`; `observe` is refused by the engine.
[[nodiscard]] Result<Plan> propose(actions::PolicyEngine& engine, Plan plan,
                                   actions::AutomationLevel level);

}  // namespace myclean::placement
