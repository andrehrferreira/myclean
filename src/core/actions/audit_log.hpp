#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {

// One row of action_log. Rows are only ever appended.
//
// Events and their outcomes:
//   transition  a plan state change (from, to): "accepted", or "rejected" with the reason
//   approval    the owner's decision on one item: "approved" or "not_approved"
//   apply, restore, purge
//               what SafeRemove did with one item: a safety::Disposition text form, the
//               reason code and "freed=<bytes>" in metrics
//   verify      the re-measured metric: an Effect text form, "<metric> before=<x> after=<y>"; or
//               "pending", "<metric> before=<x>" while it is measured again later; or "error"
//               with why it could not be measured in detail
struct LogEntry {
  WallTime time;
  PlanId plan;                     // 0 when the request named no stored plan
  std::optional<PlanItemId> item;  // set for an entry about one item
  std::string actor;               // Actor::to_string()
  std::string event;
  std::optional<PlanState> from;
  std::optional<PlanState> to;
  std::string outcome;
  std::string reason;
  std::string detail;
  std::string metrics;
};

// Appends `entry` and returns its row id. Times are stored to the second.
[[nodiscard]] Result<std::int64_t> append_log(store::Database& db, const LogEntry& entry);
// The entries of `plan`, oldest first.
[[nodiscard]] Result<std::vector<LogEntry>> plan_log(store::Database& db, PlanId plan);

// One action_log row with its id.
struct LogRow {
  std::int64_t id = 0;
  LogEntry entry;
};
// The newest `limit` entries whose id is below `before` (from the newest when nullopt), newest
// first: one page of the audit log.
[[nodiscard]] Result<std::vector<LogRow>> log_page(store::Database& db,
                                                   std::optional<std::int64_t> before,
                                                   std::size_t limit);

// The newest "verify" entry of `plan`; nullopt when it has none.
[[nodiscard]] Result<std::optional<LogEntry>> last_verification(store::Database& db, PlanId plan);
// The applied plans whose newest verification is pending, oldest first.
[[nodiscard]] Result<std::vector<PlanId>> pending_verifications(store::Database& db);

// Stores the undo data of `plan`, linked to the action_log entry `log_id`.
[[nodiscard]] Result<void> save_undo(store::Database& db, std::int64_t log_id, PlanId plan,
                                     const Undo& undo);
// The stored undo data of `plan`; fails with not_found when none was stored.
[[nodiscard]] Result<Undo> load_undo(store::Database& db, PlanId plan);

// The action parameters of `plan` (table plan_params), written once when it is submitted, so
// that the executor rebuilds exactly the action the owner approved.
[[nodiscard]] Result<void> save_params(store::Database& db, PlanId plan, std::string_view params);
// Empty when the plan was stored without parameters.
[[nodiscard]] Result<std::string> load_params(store::Database& db, PlanId plan);

}  // namespace myclean::actions
