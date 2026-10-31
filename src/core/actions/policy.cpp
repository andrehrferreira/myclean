#include "core/actions/policy.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::actions {
namespace {

// Stores the owner's decision on every item of `plan` and logs each: the items in `approved`
// are approved, the others not. Fails, for the caller to roll back, on an item not in the plan.
Result<void> record_decisions(store::Database& db, const Plan& plan,
                              std::span<const PlanItemId> approved, const Actor& actor,
                              WallTime time) {
  for (const PlanItemId item : approved) {
    if (std::ranges::find(plan.items, item, &PlanItem::id) == plan.items.end()) {
      return fail(Errc::invalid_argument, "plan " + std::to_string(plan.id.value()) +
                                              " has no item " + std::to_string(item.value()));
    }
  }
  for (const PlanItem& item : plan.items) {
    const ItemApproval decision = std::ranges::find(approved, item.id) != approved.end()
                                      ? ItemApproval::approved
                                      : ItemApproval::not_approved;
    if (Result<void> set = store::set_item_approval(db, item.id, decision); !set) {
      return set;
    }
    const Result<std::int64_t> logged =
        append_log(db, LogEntry{.time = time,
                                .plan = plan.id,
                                .item = item.id,
                                .actor = actor.to_string(),
                                .event = "approval",
                                .outcome = std::string(to_string(decision))});
    if (!logged) {
      return tl::unexpected(logged.error());
    }
  }
  return {};
}

// Why `actor` may not move a plan from `from` to `to`; nullopt when it may.
std::optional<Error> refusal(PlanState from, PlanState to, const Actor& actor) {
  const std::string edge = std::string(to_string(from)) + " -> " + std::string(to_string(to));
  if (!is_legal(from, to)) {
    return Error{.code = Errc::invalid_state, .message = "illegal transition " + edge};
  }
  if (!may_request(actor, from, to)) {
    return Error{.code = Errc::refused, .message = actor.to_string() + " may not request " + edge};
  }
  return std::nullopt;
}

}  // namespace

bool is_legal(PlanState from, PlanState to) {
  switch (from) {
    case PlanState::proposed:
      return to == PlanState::automatic || to == PlanState::awaiting_approval;
    case PlanState::automatic:
      return to == PlanState::approved;
    case PlanState::awaiting_approval:
      return to == PlanState::approved || to == PlanState::rejected;
    case PlanState::approved:
      return to == PlanState::applying;
    case PlanState::applying:
      return to == PlanState::applied || to == PlanState::failed;
    case PlanState::applied:
      return to == PlanState::verified || to == PlanState::rolled_back || to == PlanState::failed;
    case PlanState::verified:
    case PlanState::rolled_back:
    case PlanState::failed:
    case PlanState::rejected:
      return false;
  }
  return false;
}

bool may_request(const Actor& actor, PlanState from, PlanState to) {
  if (actor.kind() == Actor::Kind::mcp) {
    return from == PlanState::proposed && to == PlanState::awaiting_approval;
  }
  if (from == PlanState::awaiting_approval) {
    return actor.kind() == Actor::Kind::owner;
  }
  return true;
}

Result<std::int64_t> PolicyEngine::record(store::Database& db, const Plan& plan, PlanState to,
                                          const Actor& actor, std::string_view reason,
                                          std::optional<Error>& rejection) const {
  const std::optional<Error> refused = refusal(plan.state, to, actor);
  Result<std::int64_t> logged = append_log(
      db, LogEntry{.time = clock_->now(),
                   .plan = plan.id,
                   .actor = actor.to_string(),
                   .event = "transition",
                   .from = plan.state,
                   .to = to,
                   .outcome = refused ? "rejected" : "accepted",
                   .reason = refused ? std::string(to_string(refused->code)) : std::string(reason),
                   .detail = refused ? refused->message : std::string{}});
  if (!logged || refused) {
    rejection = refused;
    return logged;
  }
  if (Result<void> set = store::set_plan_state(db, plan.id, to); !set) {
    return tl::unexpected(set.error());
  }
  return logged;
}

Result<PlanState> PolicyEngine::state_of(PlanId id) {
  Result<Plan> plan = fail(Errc::io_error, "plan not read");
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    plan = store::get_plan(db, id);
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  return plan->state;
}

Result<void> PolicyEngine::transition(PlanId id, PlanState to, const Actor& actor,
                                      std::string_view reason) {
  std::optional<Error> rejection;
  Result<void> written = store_->write([&](store::Database& db) -> Result<void> {
    Result<Plan> plan = store::get_plan(db, id);
    if (!plan) {
      return tl::unexpected(plan.error());
    }
    if (Result<std::int64_t> logged = record(db, *plan, to, actor, reason, rejection); !logged) {
      return tl::unexpected(logged.error());
    }
    return {};
  });
  if (!written) {
    return written;
  }
  if (rejection) {
    return tl::unexpected(*rejection);
  }
  return {};
}

Result<void> PolicyEngine::append(const LogEntry& entry) {
  return store_->write([&](store::Database& db) -> Result<void> {
    if (Result<std::int64_t> logged = append_log(db, entry); !logged) {
      return tl::unexpected(logged.error());
    }
    return {};
  });
}

void PolicyEngine::settle(PlanId id, PlanState to, const Actor& actor, std::string_view reason) {
  static_cast<void>(transition(id, to, actor, reason));
}

Result<Plan> PolicyEngine::submit(Plan plan, AutomationLevel level) {
  if (level == AutomationLevel::observe) {
    return fail(Errc::refused, "a feature at level observe produces findings, never plans");
  }
  plan.state = PlanState::proposed;
  Result<Plan> stored = fail(Errc::io_error, "plan not stored");
  const Result<void> written = store_->write([&](store::Database& db) -> Result<void> {
    stored = store::add_plan(db, std::move(plan));
    if (!stored) {
      return tl::unexpected(stored.error());
    }
    const Result<std::int64_t> logged =
        append_log(db, LogEntry{.time = clock_->now(),
                                .plan = stored->id,
                                .actor = stored->proposer.to_string(),
                                .event = "transition",
                                .to = PlanState::proposed,
                                .outcome = "accepted"});
    if (!logged) {
      return tl::unexpected(logged.error());
    }
    return {};
  });
  if (!written) {
    return tl::unexpected(written.error());
  }
  const Actor& proposer = stored->proposer;
  const bool waits = level == AutomationLevel::suggest || proposer.kind() == Actor::Kind::mcp;
  const std::vector<PlanState> steps =
      waits ? std::vector<PlanState>{PlanState::awaiting_approval}
            : std::vector<PlanState>{PlanState::automatic, PlanState::approved};
  for (const PlanState step : steps) {
    if (Result<void> moved = transition(stored->id, step, proposer); !moved) {
      return tl::unexpected(moved.error());
    }
    stored->state = step;
  }
  return stored;
}

Result<PlanState> PolicyEngine::approve(PlanId id, std::span<const PlanItemId> approved,
                                        const Actor& actor) {
  const PlanState to = approved.empty() ? PlanState::rejected : PlanState::approved;
  std::optional<Error> rejection;
  const Result<void> written = store_->write([&](store::Database& db) -> Result<void> {
    Result<Plan> plan = store::get_plan(db, id);
    if (!plan) {
      return tl::unexpected(plan.error());
    }
    if (!refusal(plan->state, to, actor)) {
      if (Result<void> decided = record_decisions(db, *plan, approved, actor, clock_->now());
          !decided) {
        return decided;
      }
    }
    if (Result<std::int64_t> logged = record(db, *plan, to, actor, {}, rejection); !logged) {
      return tl::unexpected(logged.error());
    }
    return {};
  });
  if (!written) {
    return tl::unexpected(written.error());
  }
  if (rejection) {
    return tl::unexpected(*rejection);
  }
  return to;
}

Result<RunReport> PolicyEngine::execute(PlanId id, Action& action, const Actor& actor,
                                        bool auto_undo) {
  if (Result<void> started = transition(id, PlanState::applying, actor); !started) {
    return tl::unexpected(started.error());
  }
  const Result<Preview> preview = action.preview();
  if (!preview) {
    settle(id, PlanState::failed, actor, preview.error().message);
    return tl::unexpected(preview.error());
  }
  const Result<Undo> undo = action.apply(*preview);
  if (!undo) {
    settle(id, PlanState::failed, actor, undo.error().message);
    return tl::unexpected(undo.error());
  }

  // The undo data commits with the transition to applied, so it survives a restart.
  std::optional<Error> rejection;
  const Result<void> applied = store_->write([&](store::Database& db) -> Result<void> {
    Result<Plan> plan = store::get_plan(db, id);
    if (!plan) {
      return tl::unexpected(plan.error());
    }
    const Result<std::int64_t> logged = record(db, *plan, PlanState::applied, actor, {}, rejection);
    if (!logged || rejection) {
      return logged ? Result<void>{} : tl::unexpected(logged.error());
    }
    return save_undo(db, *logged, id, *undo);
  });
  if (!applied) {
    return tl::unexpected(applied.error());
  }
  if (rejection) {
    return tl::unexpected(*rejection);
  }

  RunReport report{.state = PlanState::applied, .undo = *undo};
  const Result<Verification> verification = action.verify(*undo);
  LogEntry verify_entry{.time = clock_->now(),
                        .plan = id,
                        .actor = actor.to_string(),
                        .event = "verify",
                        .outcome = "error"};
  if (!verification) {
    verify_entry.detail = verification.error().message;
  } else {
    verify_entry.outcome = std::string(to_string(verification->effect));
    verify_entry.metrics = std::format("{} before={} after={}", verification->metric,
                                       verification->before, verification->after);
  }
  if (Result<void> logged = append(verify_entry); !logged) {
    return tl::unexpected(logged.error());
  }
  if (!verification) {
    report.alert = true;
    return report;
  }
  report.verification = *verification;

  if (verification->effect != Effect::worse) {
    if (Result<void> verified = transition(id, PlanState::verified, actor); !verified) {
      return tl::unexpected(verified.error());
    }
    report.state = PlanState::verified;
    return report;
  }
  if (undo->reversible() && auto_undo) {
    if (const Result<void> undone = this->undo(id, action, actor); undone) {
      report.state = PlanState::rolled_back;
      return report;
    }
  }
  report.alert = true;
  return report;
}

Result<void> PolicyEngine::undo(PlanId id, Action& action, const Actor& actor) {
  const Result<PlanState> state = state_of(id);
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (refusal(*state, PlanState::rolled_back, actor)) {
    return transition(id, PlanState::rolled_back, actor);  // rejected and logged
  }
  Result<Undo> stored = fail(Errc::not_found, "no undo data");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        stored = load_undo(db, id);
        return {};
      });
      !read) {
    return read;
  }
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  if (Result<void> undone = action.undo(*stored); !undone) {
    const Result<void> logged =
        append(LogEntry{.time = clock_->now(),
                        .plan = id,
                        .actor = actor.to_string(),
                        .event = "undo",
                        .outcome = "failed",
                        .reason = std::string(to_string(undone.error().code)),
                        .detail = undone.error().message});
    return logged ? undone : logged;
  }
  return transition(id, PlanState::rolled_back, actor, "undone");
}

}  // namespace myclean::actions
