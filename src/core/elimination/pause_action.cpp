#include "core/elimination/pause_action.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/elimination/keeper.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"

namespace myclean::elimination {

ActionInfo PauseSuspect::describe() const {
  const std::string hours = std::to_string(period_.count()) + " hours";
  if (mode_ == PauseMode::guided) {
    return ActionInfo{
        .what = "Quit " + image_ + " for " + hours,
        .why =
            "Finds out whether it causes the copy/paste freezes: it owns a keyboard hook, so "
            "MyClean does not suspend it; quit it yourself, and the freezes while it is not "
            "running are compared with the baseline. MyClean reminds you to start it again",
        .risk = Risk::low,
        .tier = Tier::ask,
        .context = ExecutionContext::service};
  }
  return ActionInfo{.what = "Pause " + image_ + " for " + hours,
                    .why =
                        "Finds out whether it causes the copy/paste freezes: it is suspended "
                        "and resumed automatically, and the freezes during the pause are "
                        "compared with the baseline",
                    .risk = Risk::medium,
                    .tier = Tier::ask,
                    .context = ExecutionContext::service};
}

Result<Preview> PauseSuspect::preview() {
  const std::string path = "pause:" + image_;
  return Preview{
      .items = {PlanItem{.path = std::filesystem::path(std::u8string(path.begin(), path.end()))}}};
}

Result<Undo> PauseSuspect::apply(const Preview& preview) {
  if (preview.items.size() != 1) {
    return fail(Errc::invalid_argument, "a pause has exactly one item");
  }
  Result<PauseRecord> pause = pauses_->pause(plan_, image_, mode_, period_);
  if (!pause) {
    return tl::unexpected(pause.error());
  }
  return Undo{.items = {UndoItem{.item = preview.items.front().id,
                                 .data = nlohmann::json{{"pause", pause->id}}.dump()}}};
}

Result<Verification> PauseSuspect::verify(const Undo& /*undo*/) {
  Result<std::optional<PauseRecord>> pause = pauses_->record(plan_);
  if (!pause) {
    return tl::unexpected(pause.error());
  }
  if (!*pause) {
    return fail(Errc::not_found, "plan " + std::to_string(plan_.value()) + " has no pause");
  }
  const PauseRecord& record = **pause;
  const double before = record.baseline.per_day().value_or(0.0);
  if (record.active()) {
    return Verification{.metric = "freezes_per_day", .before = before, .pending = true};
  }
  const std::optional<double> after = record.during ? record.during->per_day() : std::nullopt;
  if (!after) {
    return fail(Errc::invalid_state, record.mode == PauseMode::guided
                                         ? record.image + " ran during the whole pause"
                                         : "the pause of " + record.image + " observed no time");
  }
  return Verification{.metric = "freezes_per_day",
                      .before = before,
                      .after = *after,
                      .effect = *after < before ? Effect::better : Effect::unchanged};
}

Result<void> PauseSuspect::undo(const Undo& /*undo*/) {
  return pauses_->end(plan_, EndReason::undone);
}

}  // namespace myclean::elimination
