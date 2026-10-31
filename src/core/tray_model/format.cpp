#include "core/tray_model/format.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace myclean::tray_model {

namespace {

// Fixed-point text with enough decimals for three significant digits (below 1000).
std::string three_digits(double value) {
  if (value < 10.0) {
    return std::format("{:.2f}", value);
  }
  if (value < 100.0) {
    return std::format("{:.1f}", value);
  }
  return std::format("{:.0f}", value);
}

}  // namespace

std::string format_bytes(double bytes) {
  static constexpr std::array<std::string_view, 5> units = {"KB", "MB", "GB", "TB", "PB"};
  if (!std::isfinite(bytes) || bytes < 0.0) {
    return std::string(not_available);
  }
  if (bytes < 1024.0) {
    return std::format("{:.0f} B", bytes);
  }
  double value = bytes / 1024.0;
  std::size_t unit = 0;
  // 999.5 and above would print as "1000", so it moves to the next unit.
  while (value >= 999.5 && unit + 1 < units.size()) {
    value /= 1024.0;
    ++unit;
  }
  return three_digits(value) + " " + std::string(units.at(unit));
}

std::string format_bytes(std::optional<double> bytes) {
  return bytes ? format_bytes(*bytes) : std::string(not_available);
}

std::string format_rate(std::optional<double> bytes_per_second) {
  return bytes_per_second ? format_bytes(*bytes_per_second) + "/s" : std::string(not_available);
}

std::string format_percent(std::optional<double> percent) {
  if (!percent || !std::isfinite(*percent)) {
    return std::string(not_available);
  }
  if (*percent > 0.0 && *percent < 0.95) {
    return std::format("{:.1f} %", *percent);
  }
  return std::format("{:.0f} %", *percent);
}

std::string format_milliseconds(std::optional<double> milliseconds) {
  if (!milliseconds || !std::isfinite(*milliseconds) || *milliseconds < 0.0) {
    return std::string(not_available);
  }
  return three_digits(*milliseconds) + " ms";
}

std::string format_count(std::optional<double> count) {
  if (!count || !std::isfinite(*count)) {
    return std::string(not_available);
  }
  if (std::abs(*count - std::round(*count)) < 0.05) {
    return std::format("{:.0f}", *count);
  }
  return std::format("{:.1f}", *count);
}

std::string format_days(double days) {
  if (days < 1.0) {
    return "less than 1 day";
  }
  const double whole = std::floor(days);
  return whole == 1.0 ? "1 day" : std::format("{:.0f} days", whole);
}

std::string format_time_of_day(std::chrono::local_seconds time) {
  return std::format("{:%H:%M:%S}", time);
}

std::string format_date_time(std::chrono::local_seconds time) {
  return std::format("{:%Y-%m-%d %H:%M}", time);
}

}  // namespace myclean::tray_model
