#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"

// The memory-leak detector (optimizer spec, "Memory-Leak Detection"): it reads a program's
// private-bytes trend and reports to the alert engine; it never acts on a process.
namespace myclean::resident {

// The alert reading: whole hours of steady growth, 0 when there is none (data/alerts.toml rule
// process.memory-leak).
inline constexpr std::string_view growth_metric = "process.private_growth_hours";

// An hour of the trend counts only with at least this many of its 12 buckets.
inline constexpr std::size_t min_hour_buckets = 6;
// A trend whose newest bucket is older than this is of a program that no longer runs.
inline constexpr std::chrono::minutes stale_after{15};
// Growth over the run that makes it a leak: at least this much, and this share of the start.
inline constexpr Bytes min_leak_growth{64ULL << 20U};
inline constexpr double min_leak_ratio = 0.10;

struct Growth {
  int hours = 0;              // whole hours of steady growth, ending at the newest bucket
  std::vector<Bytes> hourly;  // the median private bytes of each of those hours, oldest first

  friend bool operator==(const Growth&, const Growth&) = default;
};

// The steady growth at `now` in `trend` (5-minute buckets, oldest first). The trend is cut into
// hours back from its newest bucket; the run is the newest hours whose medians rise strictly,
// hour after hour, each hour with at least min_hour_buckets buckets. It is a leak when it spans
// at least 2 hours, every bucket of its last hour is above every bucket of its first (it never
// returned to its earlier level), and the median rose by at least min_leak_growth and
// min_leak_ratio of the first hour's. A spike that falls back within the hour breaks the run.
// nullopt when there is no leak, or the newest bucket is stale_after old.
[[nodiscard]] std::optional<Growth> steady_growth(std::span<const Point> trend, WallTime now);

// The reading of `key` for the alert engine: growth_metric, entity `key`, the hours of
// `growth` (0 without), and a detail that names the program and gives the trend.
[[nodiscard]] alerts::Reading leak_reading(std::string_view key,
                                           const std::optional<Growth>& growth);

}  // namespace myclean::resident
