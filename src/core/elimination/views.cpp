#include "core/elimination/views.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/elimination/pause_action.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::elimination {
namespace {

using Json = nlohmann::json;

Json rate_json(const Rate& rate) {
  const std::optional<double> per_day = rate.per_day();
  return Json{{"incidents", rate.incidents},
              {"hours", rate.hours},
              {"per_day", per_day ? Json(*per_day) : Json(nullptr)}};
}

Json pause_json(const PauseRecord& pause) {
  Json change = nullptr;
  const std::optional<double> before = pause.baseline.per_day();
  const std::optional<double> during = pause.during ? pause.during->per_day() : std::nullopt;
  if (before && during) {
    change = "same";
    if (*during < *before) {
      change = "lower";
    } else if (*during > *before) {
      change = "higher";
    }
  }
  return Json{
      {"plan", pause.plan.value()},
      {"image", pause.image},
      {"mode", to_string(pause.mode)},
      {"started", store::to_unix_seconds(pause.started)},
      {"ends", store::to_unix_seconds(pause.ends)},
      {"restored", pause.restored ? Json(store::to_unix_seconds(*pause.restored)) : Json(nullptr)},
      {"end_reason", pause.reason ? Json(std::string(to_string(*pause.reason))) : Json(nullptr)},
      {"baseline", rate_json(pause.baseline)},
      {"during", pause.during ? rate_json(*pause.during) : Json(nullptr)},
      {"change", std::move(change)}};
}

bool runs(const std::shared_ptr<const telemetry::ProcessView>& view, const std::string& image) {
  return view != nullptr && std::ranges::any_of(view->rows, [&](const telemetry::ProcessRow& row) {
           return same_image(row.info.image, image);
         });
}

}  // namespace

Result<Json> pauses_answer(store::Store& store,
                           const std::shared_ptr<const telemetry::ProcessView>& view,
                           WallTime now) {
  Result<std::vector<input::FreezeGroup>> groups = input::freeze_summary(store);
  if (!groups) {
    return tl::unexpected(groups.error());
  }
  std::vector<input::StoredIncident> incidents;
  for (input::FreezeGroup& group : *groups) {
    std::ranges::move(group.incidents, std::back_inserter(incidents));
  }
  Result<std::vector<WallTime>> times = input::incident_times(store);
  if (!times) {
    return tl::unexpected(times.error());
  }
  Result<std::vector<Span>> periods = fail(Errc::io_error, "not read");
  Result<std::vector<PauseRecord>> pauses = fail(Errc::io_error, "not read");
  if (Result<void> read = store.read([&](store::Database& db) -> Result<void> {
        periods = pause_periods(db);
        pauses = list_pauses(db, max_listed_pauses);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!periods) {
    return tl::unexpected(periods.error());
  }
  if (!pauses) {
    return tl::unexpected(pauses.error());
  }
  const std::optional<Rate> base = baseline(*times, now, *periods);
  const auto active = std::ranges::find_if(*pauses, &PauseRecord::active);

  Json suspects = Json::array();
  Json offer = nullptr;
  for (const Suspect& suspect : ranked_suspects(incidents)) {
    const bool running = runs(view, suspect.image);
    suspects.push_back(Json{{"id", suspect.id},
                            {"image", suspect.image},
                            {"hook", suspect.hook},
                            {"mode", to_string(suspect.mode())},
                            {"running", view == nullptr ? Json(nullptr) : Json(running)}});
    // The highest-ranked running suspect not compared yet, once freezes have a baseline.
    const bool compared = std::ranges::any_of(*pauses, [&](const PauseRecord& pause) {
      return same_image(pause.image, suspect.image) && pause.during && pause.during->hours > 0.0;
    });
    if (offer.is_null() && running && !compared && base && base->incidents > 0 &&
        active == pauses->end()) {
      offer = Json{{"suspect", suspect.id},
                   {"image", suspect.image},
                   {"mode", to_string(suspect.mode())},
                   {"hours", default_pause.count()}};
    }
  }
  Json listed = Json::array();
  for (const PauseRecord& pause : *pauses) {
    listed.push_back(pause_json(pause));
  }
  Json active_json = nullptr;
  if (active != pauses->end()) {
    active_json = pause_json(*active);
    active_json["running"] = view == nullptr ? Json(nullptr) : Json(runs(view, active->image));
  }
  return Json{
      {"collecting", !base.has_value()},  {"baseline", base ? rate_json(*base) : Json(nullptr)},
      {"suspects", std::move(suspects)},  {"offer", std::move(offer)},
      {"active", std::move(active_json)}, {"pauses", std::move(listed)}};
}

}  // namespace myclean::elimination
