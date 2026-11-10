#include "core/resident/leak.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"

namespace myclean::resident {
namespace {

std::uint64_t median(std::vector<std::uint64_t> values) {
  std::ranges::sort(values);
  const std::size_t middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

double mib(Bytes bytes) {
  return static_cast<double>(bytes.count()) / static_cast<double>(1ULL << 20U);
}

}  // namespace

std::optional<Growth> steady_growth(std::span<const Point> trend, WallTime now) {
  if (trend.empty() || now - trend.back().time > stale_after) {
    return std::nullopt;
  }
  // The buckets of each hour back from the newest bucket, newest hour first.
  const WallTime end = trend.back().time;
  std::vector<std::vector<std::uint64_t>> hours;
  std::size_t next = trend.size();
  while (next > 0) {
    const WallTime lower = end - std::chrono::hours{static_cast<std::int64_t>(hours.size()) + 1};
    std::vector<std::uint64_t> values;
    while (next > 0 && trend[next - 1].time > lower) {
      values.push_back(trend[--next].private_bytes.count());
    }
    if (values.size() < min_hour_buckets) {
      break;
    }
    hours.push_back(std::move(values));
  }
  std::vector<std::uint64_t> medians;
  for (const std::vector<std::uint64_t>& hour : hours) {
    if (!medians.empty() && median(hour) >= medians.back()) {
      break;
    }
    medians.push_back(median(hour));
  }
  const std::size_t run = medians.size();
  if (run < 2) {
    return std::nullopt;
  }
  const std::uint64_t first = medians.back();
  const std::uint64_t last = medians.front();
  const bool left_earlier_level =
      std::ranges::min(hours.front()) > std::ranges::max(hours[run - 1]);
  const std::uint64_t growth = last - first;
  if (!left_earlier_level || growth < min_leak_growth.count() ||
      static_cast<double>(growth) < min_leak_ratio * static_cast<double>(first)) {
    return std::nullopt;
  }
  Growth found{.hours = static_cast<int>(run)};
  for (const std::uint64_t median : std::views::reverse(medians)) {
    found.hourly.emplace_back(median);
  }
  return found;
}

alerts::Reading leak_reading(std::string_view key, const std::optional<Growth>& growth) {
  alerts::Reading reading{.metric = std::string(growth_metric), .entity = std::string(key)};
  if (!growth || growth->hourly.size() < 2) {
    return reading;
  }
  reading.value = growth->hours;
  const Bytes first = growth->hourly.front();
  const Bytes last = growth->hourly.back();
  std::string hourly;
  for (const Bytes median : growth->hourly) {
    hourly += std::format("{}{:.0f}", hourly.empty() ? "" : ", ", mib(median));
  }
  reading.detail = std::format(
      "{}: private bytes rose from {:.0f} MiB to {:.0f} MiB in {} hours (+{:.0f} MiB per hour); "
      "hourly medians: {} MiB",
      key, mib(first), mib(last), growth->hours,
      (mib(last) - mib(first)) / static_cast<double>(growth->hours - 1), hourly);
  return reading;
}

}  // namespace myclean::resident
