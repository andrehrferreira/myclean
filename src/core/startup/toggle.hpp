#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/startup/startup.hpp"

namespace myclean::startup {

// The name plans of this action carry.
inline constexpr std::string_view toggle_action_name = "SetStartupItems";

// Disables or enables startup items the way Windows does (startup spec, "Disable and Enable
// Through Native Mechanisms", "Exact Startup Undo"): a Run, Run32 or Startup-folder item through
// its StartupApproved value, a task through its Enabled flag, a service through its start type
// (Manual to disable). The Run value, the shortcut, the task and the service always stay. Tier
// ask, run by the service; both references must outlive the action.
class StartupToggle final : public Action {
 public:
  StartupToggle(IStartupSystem& system, const os::IClock& clock, std::vector<std::string> ids,
                bool enable)
      : system_(&system), clock_(&clock), ids_(std::move(ids)), enable_(enable) {}

  [[nodiscard]] ActionInfo describe() const override;
  // One item per id, in the given order, its path the id. Fails with not_found for an id the
  // inventory does not list and with invalid_argument for a RunOnce item, a service to enable
  // (only undo enables a service again), or a duplicate id.
  [[nodiscard]] Result<Preview> preview() override;
  // Changes each item of `preview` not yet in the requested state; undo holds, per changed item,
  // the state before (the StartupApproved value byte for byte or its absence, the Enabled flag,
  // the start type with its delayed flag) and the state written. When one change fails, those
  // made are reversed and the error returned. Fails with invalid_state when every item already
  // is in the requested state.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "startup_items_changed": items of `undo` in the state apply wrote, before (0) and now;
  // better when all of them are.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Restores the state before of every item of `undo`. Fails with invalid_state, changing
  // nothing, when an item is no longer in the state apply wrote (changed meanwhile, or gone).
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IStartupSystem* system_;
  const os::IClock* clock_;
  std::vector<std::string> ids_;
  bool enable_;
};

}  // namespace myclean::startup
