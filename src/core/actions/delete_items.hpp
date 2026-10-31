#pragma once

#include <utility>

#include "core/actions/item_action.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"

namespace myclean::actions {

// Deletes the regenerable items of one stored plan through SafeRemove (tier auto). Not
// reversible: the files are rebuilt by whoever needs them. Every item outcome, refusals
// included, is written to action_log as event "apply".
class DeleteItems final : public ItemAction {
 public:
  DeleteItems(const Environment& environment, PlanId plan, Actor actor)
      : ItemAction(environment, plan, std::move(actor)) {}

  [[nodiscard]] ActionInfo describe() const override;
  // Undo lists every removed (or partly removed) item; not_reversible is "regenerates".
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Always fails with not_reversible: "not reversible: regenerates".
  [[nodiscard]] Result<void> undo(const Undo& undo) override;
};

}  // namespace myclean::actions
