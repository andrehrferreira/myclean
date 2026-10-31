#pragma once

#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::store {

// Findings and plans (tables finding, plan, plan_item). Every function is atomic on its own and
// joins the caller's transaction when called inside Store::write, so a plan change and its
// action_log entry can be committed together. Times are stored to the second.

// Stores `finding` and returns its new id; finding.id is ignored.
[[nodiscard]] Result<FindingId> add_finding(Database& db, const Finding& finding);
// Fails with not_found when no finding has `id`.
[[nodiscard]] Result<Finding> get_finding(Database& db, FindingId id);

// Stores `plan` with its items and returns it with the new plan and item ids; existing ids in
// `plan` are ignored.
[[nodiscard]] Result<Plan> add_plan(Database& db, Plan plan);
// The plan with its items in insertion order. Fails with not_found when no plan has `id`.
[[nodiscard]] Result<Plan> get_plan(Database& db, PlanId id);
// Records the new state; the legal transitions are enforced by the policy state machine.
[[nodiscard]] Result<void> set_plan_state(Database& db, PlanId id, PlanState state);
[[nodiscard]] Result<void> set_item_approval(Database& db, PlanItemId id, ItemApproval approval);

}  // namespace myclean::store
