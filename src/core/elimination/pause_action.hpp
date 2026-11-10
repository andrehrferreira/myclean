#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "core/elimination/keeper.hpp"
#include "core/elimination/suspects.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"

namespace myclean::elimination {

// The name plans of this action carry.
inline constexpr std::string_view pause_action_name = "PauseSuspect";

// A pause asked for up to this long.
inline constexpr std::chrono::hours default_pause{24};
inline constexpr std::chrono::hours max_pause{168};

// One time-boxed pause of a freeze suspect (input-watchdog spec, "Pause Requires Tray Approval"),
// run by the service once the owner approved its plan, tier ask, through `pauses`:
//   preview  one item "pause:<image>";
//   apply    starts the pause (IPauses::pause): a suspend pause suspends the suspect, a guided one
//            asks the owner to quit it; undo data {"pause": id}, reversible;
//   verify   metric "freezes_per_day": the baseline rate before, the rate during the pause after;
//            pending while the pause runs; better when the rate fell, unchanged otherwise (a pause
//            that is over has nothing to undo). Fails when the pause observed no hour (the owner
//            never quit the suspect during a guided pause);
//   undo     ends the pause now (EndReason::undone); nothing to do once it is over.
// `pauses` must outlive the action.
class PauseSuspect final : public Action {
 public:
  PauseSuspect(IPauses& pauses, PlanId plan, std::string image, PauseMode mode,
               std::chrono::hours period)
      : pauses_(&pauses), plan_(plan), image_(std::move(image)), mode_(mode), period_(period) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  IPauses* pauses_;
  PlanId plan_;
  std::string image_;
  PauseMode mode_;
  std::chrono::hours period_;
};

}  // namespace myclean::elimination
