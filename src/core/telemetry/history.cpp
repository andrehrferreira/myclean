#include "core/telemetry/history.hpp"

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "core/store/time_series.hpp"
#include "core/telemetry/metrics.hpp"

namespace myclean::telemetry {

std::string_view to_string(Resolution resolution) {
  switch (resolution) {
    case Resolution::raw:
      return "raw";
    case Resolution::minute:
      return "minute";
    case Resolution::hour:
      return "hour";
  }
  return "unknown";
}

Result<History> query_history(store::Store& store, const SeriesKey& key, WallTime from, WallTime to,
                              WallTime now) {
  if (from >= now - store::SeriesBuffer::raw_window) {
    History history{.resolution = Resolution::raw};
    for (const Sample& sample : store.raw(key)) {
      if (sample.time >= from && sample.time < to) {
        history.points.push_back(store::Aggregate{.time = sample.time,
                                                  .min = sample.value,
                                                  .max = sample.value,
                                                  .mean = sample.value,
                                                  .count = 1});
      }
    }
    return history;
  }
  const bool minutes = from >= now - store::Store::minute_retention;
  Result<std::vector<store::Aggregate>> points =
      minutes ? store.minutes(key, from, to) : store.hours(key, from, to);
  if (!points) {
    return tl::unexpected(points.error());
  }
  return History{.resolution = minutes ? Resolution::minute : Resolution::hour,
                 .points = std::move(*points)};
}

FillProjection project_fill(std::span<const store::Aggregate> free_space) {
  if (free_space.size() < 2 || free_space.back().time - free_space.front().time < fill_min_span) {
    return {};
  }
  using Days = std::chrono::duration<double, std::chrono::days::period>;
  const WallTime origin = free_space.front().time;
  double mean_x = 0.0;
  double mean_y = 0.0;
  for (const store::Aggregate& point : free_space) {
    mean_x += Days(point.time - origin).count();
    mean_y += point.mean;
  }
  const auto count = static_cast<double>(free_space.size());
  mean_x /= count;
  mean_y /= count;
  double covariance = 0.0;
  double variance = 0.0;
  for (const store::Aggregate& point : free_space) {
    const double x = Days(point.time - origin).count() - mean_x;
    covariance += x * (point.mean - mean_y);
    variance += x * x;
  }
  // Bytes per day. variance > 0: the points span at least fill_min_span.
  const double slope = covariance / variance;
  if (slope >= 0.0) {
    return FillProjection{.state = FillState::not_filling, .bytes_per_day = slope};
  }
  return FillProjection{.state = FillState::filling,
                        .bytes_per_day = slope,
                        .days_until_full = free_space.back().mean / -slope};
}

Result<FillProjection> project_volume_fill(store::Store& store, std::string_view entity,
                                           WallTime now) {
  const SeriesKey key{.metric = std::string(metric::volume_free), .entity = std::string(entity)};
  const Result<std::vector<store::Aggregate>> points =
      store.minutes(key, now - fill_trend_window, now);
  if (!points) {
    return tl::unexpected(points.error());
  }
  return project_fill(*points);
}

}  // namespace myclean::telemetry
