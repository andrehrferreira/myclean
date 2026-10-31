#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"

namespace myclean::actions {

// How far a feature may go on its own (docs/architecture.md section 6).
enum class AutomationLevel {
  observe,    // findings only: never a plan
  suggest,    // plans wait for the owner's approval
  automatic,  // plans are approved without the owner
};

// True when `from -> to` is an edge of the plan state machine:
//   proposed -> auto | awaiting_approval;  auto -> approved;
//   awaiting_approval -> approved | rejected;  approved -> applying;
//   applying -> applied | failed;  applied -> verified | rolled_back | failed
[[nodiscard]] bool is_legal(PlanState from, PlanState to);

// True when `actor` may request the legal transition `from -> to`. Only the owner decides on a
// plan awaiting approval (approved or rejected). An `mcp:*` actor may only make a proposal
// wait for the owner (proposed -> awaiting_approval): it can never cause `approved`, also not
// through `auto`.
[[nodiscard]] bool may_request(const Actor& actor, PlanState from, PlanState to);

// What execute() did.
struct RunReport {
  PlanState state = PlanState::applied;  // where the plan ended
  Undo undo;                             // as returned by apply and stored in undo_blob
  std::optional<Verification> verification;
  // The targeted metric got worse (or could not be measured) and the change was kept: the owner
  // must be told, and may undo it.
  bool alert = false;
};

// The policy state machine over stored plans. Every transition, accepted or rejected, is written
// to action_log with its actor; an accepted one commits with its log entry in one transaction.
// `store` (opened with action_migrations()) and `clock` must outlive the engine.
class PolicyEngine {
 public:
  PolicyEngine(store::Store& store, const os::IClock& clock) : store_(&store), clock_(&clock) {}

  // Stores `plan` as proposed by plan.proposer (its state and ids are ignored) and moves it on
  // by `level`: suggest -> awaiting_approval; automatic -> auto -> approved. A proposal of an
  // `mcp:*` actor always waits for the owner. `observe` is refused and stores nothing.
  [[nodiscard]] Result<Plan> submit(Plan plan, AutomationLevel level);

  // Moves plan `id` to `to`. An illegal transition fails with invalid_state, one `actor` may not
  // request with refused; both are logged and leave the plan unchanged.
  [[nodiscard]] Result<void> transition(PlanId id, PlanState to, const Actor& actor,
                                        std::string_view reason = {});

  // The owner's decision on a plan awaiting approval: the listed items are approved, every
  // other item is not. Approving none rejects the plan. Returns the new state. Fails with
  // invalid_argument, changing nothing, when a listed item is not in the plan.
  [[nodiscard]] Result<PlanState> approve(PlanId id, std::span<const PlanItemId> approved,
                                          const Actor& actor);

  // Runs approved plan `id` with `action` (built for that plan): applying, apply, then applied
  // with the undo data stored in the same transaction, then verify. Equal or better -> verified.
  // Worse -> undo and rolled_back when the change is reversible and `auto_undo` allows it,
  // otherwise the plan stays applied and the report raises an alert. A failed apply -> failed.
  [[nodiscard]] Result<RunReport> execute(PlanId id, Action& action, const Actor& actor,
                                          bool auto_undo);

  // Reverses applied plan `id` with its stored undo data (also after a restart), then moves it
  // to rolled_back. Fails, leaving the plan applied, when the action cannot undo it.
  [[nodiscard]] Result<void> undo(PlanId id, Action& action, const Actor& actor);

 private:
  // Checks and records one transition of `plan` inside the caller's transaction. A rejected
  // one is logged too and reported through `rejection`, so that its log entry still commits.
  // Returns the id of the log entry.
  [[nodiscard]] Result<std::int64_t> record(store::Database& db, const Plan& plan, PlanState to,
                                            const Actor& actor, std::string_view reason,
                                            std::optional<Error>& rejection) const;
  [[nodiscard]] Result<PlanState> state_of(PlanId id);
  // Appends one log entry in its own transaction.
  [[nodiscard]] Result<void> append(const LogEntry& entry);
  // transition() whose failure is reported in the log only: used where a step already failed.
  void settle(PlanId id, PlanState to, const Actor& actor, std::string_view reason);

  store::Store* store_;
  const os::IClock* clock_;
};

}  // namespace myclean::actions
