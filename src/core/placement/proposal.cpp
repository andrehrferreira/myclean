#include "core/placement/proposal.hpp"

#include <string>
#include <utility>

#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::placement {

Result<Plan> plan_of(Action& action, std::string action_name, const Actor& proposer, WallTime now) {
  Result<Preview> preview = action.preview();
  if (!preview) {
    return tl::unexpected(preview.error());
  }
  return Plan{.action = std::move(action_name),
              .proposer = proposer,
              .created_at = now,
              .items = std::move(preview->items)};
}

Result<Plan> propose(actions::PolicyEngine& engine, Plan plan, actions::AutomationLevel level) {
  if (level == actions::AutomationLevel::automatic) {
    level = actions::AutomationLevel::suggest;
  }
  return engine.submit(std::move(plan), level);
}

}  // namespace myclean::placement
