#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean {

enum class Risk { low, medium, high };

// How a cleaning item is handled (docs/analysis/myclean-planning/05-safety-model.md, F-030).
enum class Tier {
  automatic,  // text form "auto": regenerable; deleted on schedule
  ask,        // quarantined after owner approval
  report,     // shown only; the owner deletes it explicitly
};

// The security context an action must run in.
enum class ExecutionContext {
  service,  // text form "svc": the LocalSystem service
  user,     // the owner's own token, in the tray
};

[[nodiscard]] std::string_view to_string(Risk risk);
[[nodiscard]] std::optional<Risk> parse_risk(std::string_view text);
[[nodiscard]] std::string_view to_string(Tier tier);
[[nodiscard]] std::optional<Tier> parse_tier(std::string_view text);
[[nodiscard]] std::string_view to_string(ExecutionContext context);
[[nodiscard]] std::optional<ExecutionContext> parse_execution_context(std::string_view text);

struct ActionInfo {
  std::string what;
  std::string why;
  Risk risk = Risk::low;
  Tier tier = Tier::report;
  ExecutionContext context = ExecutionContext::service;
};

// The exact effect an action will have. apply() acts on these items and no others.
struct Preview {
  std::vector<PlanItem> items;

  [[nodiscard]] Bytes total_size() const;
};

// What one item needs to be restored. The payload is written and read only by the action.
struct UndoItem {
  PlanItemId item;
  std::string data;
};

// Undo data returned by apply() and stored in undo_blob before the plan becomes `applied`.
struct Undo {
  std::vector<UndoItem> items;
  std::string not_reversible;  // why the change cannot be undone ("regenerates"); empty if it can

  [[nodiscard]] bool reversible() const { return not_reversible.empty(); }
};

// Direction of the targeted metric after apply, as judged by the action.
enum class Effect { better, unchanged, worse };

[[nodiscard]] std::string_view to_string(Effect effect);
[[nodiscard]] std::optional<Effect> parse_effect(std::string_view text);

struct Verification {
  std::string metric;
  double before = 0.0;
  double after = 0.0;
  Effect effect = Effect::unchanged;
};

// Every change MyClean makes implements this contract (docs/architecture.md section 6).
class Action {
 public:
  virtual ~Action() = default;
  Action(const Action&) = delete;
  Action& operator=(const Action&) = delete;
  Action(Action&&) = delete;
  Action& operator=(Action&&) = delete;

  // What, why, risk, tier and execution context.
  [[nodiscard]] virtual ActionInfo describe() const = 0;
  // The exact items, bytes or settings the action would change.
  [[nodiscard]] virtual Result<Preview> preview() = 0;
  // Re-validates every item of `preview`, executes, and returns the undo data of what changed.
  [[nodiscard]] virtual Result<Undo> apply(const Preview& preview) = 0;
  // Measures the targeted metric again.
  [[nodiscard]] virtual Result<Verification> verify(const Undo& undo) = 0;
  // Reverses the change; fails with Errc::not_reversible and the reason when it cannot.
  [[nodiscard]] virtual Result<void> undo(const Undo& undo) = 0;

 protected:
  Action() = default;
};

}  // namespace myclean
