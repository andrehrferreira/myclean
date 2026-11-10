#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"

namespace myclean::startup {

// The name plans of this action carry.
inline constexpr std::string_view toggle_action_name = "SetStartupItems";

// The boots Windows measured (table boot_time, boot_times.hpp), oldest first: the logon
// durations a disable is verified with.
using BootSource = std::function<Result<std::vector<BootRecord>>()>;

// A logon after a disable counts as longer or shorter only beyond this share of the logon before
// it: logon times vary from boot to boot.
inline constexpr double logon_tolerance = 0.10;

// Disables or enables startup items the way Windows does (startup spec, "Disable and Enable
// Through Native Mechanisms", "Exact Startup Undo"): a Run, Run32 or Startup-folder item through
// its StartupApproved value, a task through its Enabled flag, a service through its start type
// (Manual to disable). The Run value, the shortcut, the task and the service always stay. Tier
// ask, run by the service; both references must outlive the action.
class StartupToggle final : public Action {
 public:
  // `boots` (may be empty: no logon measurement) gives the logon durations a disable is verified
  // with.
  StartupToggle(IStartupSystem& system, const os::IClock& clock, std::vector<std::string> ids,
                bool enable, BootSource boots = {})
      : system_(&system),
        clock_(&clock),
        ids_(std::move(ids)),
        enable_(enable),
        boots_(std::move(boots)) {}

  [[nodiscard]] ActionInfo describe() const override;
  // One item per id, in the given order, its path the id. Fails with not_found for an id the
  // inventory does not list and with invalid_argument for a RunOnce item, a service to enable
  // (only undo enables a service again), or a duplicate id.
  [[nodiscard]] Result<Preview> preview() override;
  // Changes each item of `preview` not yet in the requested state; undo holds, per changed item,
  // the state before (the StartupApproved value byte for byte or its absence, the Enabled flag,
  // the start type with its delayed flag), the state written and when it was written. When one
  // change fails, those made are reversed and the error returned. Fails with invalid_state when
  // every item already is in the requested state.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // First whether every item of `undo` is still in the state apply wrote: when one is not, metric
  // "startup_items_changed" (before 0, after how many are) is worse. Then, for a disable with a
  // boot source, metric "logon_seconds" (optimizer spec, "Before-and-After Verification"): the
  // median logon of the change_boots boots that started before the change against the median of
  // those after it, lower_is_better with logon_tolerance; pending until a boot after the change
  // is recorded. Without a boot before the change, or for an enable, "startup_items_changed" is
  // better.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Restores the state before of every item of `undo`. Fails with invalid_state, changing
  // nothing, when an item is no longer in the state apply wrote (changed meanwhile, or gone).
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IStartupSystem* system_;
  const os::IClock* clock_;
  std::vector<std::string> ids_;
  bool enable_;
  BootSource boots_;
};

}  // namespace myclean::startup
