#include "core/actions/delete_items.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {

ActionInfo DeleteItems::describe() const {
  return {.what = "Delete the regenerable files of plan " + std::to_string(plan_id().value()),
          .why = "they take space and are rebuilt when needed",
          .risk = Risk::low,
          .tier = Tier::automatic,
          .context = ExecutionContext::service};
}

Result<Undo> DeleteItems::apply(const Preview& preview) {
  Result<Selection> selection = select(preview);
  if (!selection) {
    return tl::unexpected(selection.error());
  }
  std::vector<PlanItemId> ids;
  ids.reserve(selection->items.size());
  for (const PlanItem& item : selection->items) {
    ids.push_back(item.id);
  }
  const std::vector<safety::ItemOutcome> outcomes =
      environment().safe_remove->remove(plan_id(), ids);

  Undo undo{.not_reversible = "regenerates"};
  const Result<void> logged = environment().store->write([&](store::Database& db) -> Result<void> {
    for (const safety::ItemOutcome& refused : selection->refused) {
      if (Result<std::int64_t> row = append_log(db, outcome_entry("apply", refused, {})); !row) {
        return tl::unexpected(row.error());
      }
    }
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
      const safety::ItemOutcome& outcome = outcomes[index];
      const Result<std::int64_t> row =
          append_log(db, outcome_entry("apply", outcome, selection->items[index].path));
      if (!row) {
        return tl::unexpected(row.error());
      }
    }
    return {};
  });
  if (!logged) {
    return tl::unexpected(logged.error());
  }
  for (const safety::ItemOutcome& outcome : outcomes) {
    if (outcome.disposition == safety::Disposition::removed ||
        outcome.disposition == safety::Disposition::partial) {
      undo.items.push_back({.item = outcome.item, .data = safety::to_utf8(outcome.path)});
    }
  }
  return undo;
}

Result<void> DeleteItems::undo(const Undo& undo) {
  return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
}

}  // namespace myclean::actions
