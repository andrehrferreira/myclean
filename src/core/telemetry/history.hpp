#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "core/store/time_series.hpp"

namespace myclean::telemetry {

enum class Resolution { raw, minute, hour };

[[nodiscard]] std::string_view to_string(Resolution resolution);

struct History {
  Resolution resolution = Resolution::raw;
  // Oldest first. A raw sample is a point with min = max = mean = its value and count 1.
  std::vector<store::Aggregate> points;
};

// The points of `key` in [from, to), at the finest resolution whose retention covers `from`:
// raw 1 s samples back to 1 h before `now`, minute aggregates back to 30 days, hour aggregates
// beyond.
[[nodiscard]] Result<History> query_history(store::Store& store, const SeriesKey& key,
                                            WallTime from, WallTime to, WallTime now);

enum class FillState { insufficient_data, not_filling, filling };

struct FillProjection {
  FillState state = FillState::insufficient_data;
  double bytes_per_day = 0.0;             // trend of free space; negative while filling
  std::optional<double> days_until_full;  // set only when filling
};

// The free-space trend is fitted over this window...
inline constexpr std::chrono::days fill_trend_window{7};
// ...and needs points spanning at least this long.
inline constexpr std::chrono::hours fill_min_span{1};

// Least-squares trend of free space (point means, in bytes) and the days until it reaches 0 at
// that rate, from the newest point.
[[nodiscard]] FillProjection project_fill(std::span<const store::Aggregate> free_space);

// project_fill() over the volume.free minute aggregates of volume `entity` ("C:") in the trend
// window before `now`.
[[nodiscard]] Result<FillProjection> project_volume_fill(store::Store& store,
                                                         std::string_view entity, WallTime now);

}  // namespace myclean::telemetry
