#include "core/tray_model/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace myclean::tray_model {

namespace {

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

bool natural_less(std::string_view left, std::string_view right) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() && j < right.size()) {
    if (is_digit(left[i]) && is_digit(right[j])) {
      const std::size_t start_i = i;
      const std::size_t start_j = j;
      while (i < left.size() && is_digit(left[i])) {
        ++i;
      }
      while (j < right.size() && is_digit(right[j])) {
        ++j;
      }
      std::string_view a = left.substr(start_i, i - start_i);
      std::string_view b = right.substr(start_j, j - start_j);
      a.remove_prefix(std::min(a.find_first_not_of('0'), a.size()));
      b.remove_prefix(std::min(b.find_first_not_of('0'), b.size()));
      if (a.size() != b.size()) {
        return a.size() < b.size();
      }
      if (a != b) {
        return a < b;
      }
      continue;
    }
    if (left[i] != right[j]) {
      return left[i] < right[j];
    }
    ++i;
    ++j;
  }
  return left.size() - i < right.size() - j;
}

std::string_view to_string(AlertSeverity severity) {
  switch (severity) {
    case AlertSeverity::info:
      return "info";
    case AlertSeverity::warning:
      return "warning";
    case AlertSeverity::critical:
      break;
  }
  return "critical";
}

std::string_view to_string(DriveStatus status) {
  switch (status) {
    case DriveStatus::unknown:
      return "unknown";
    case DriveStatus::good:
      return "good";
    case DriveStatus::warning:
      return "warning";
    case DriveStatus::failing:
      break;
  }
  return "failing";
}

void MetricsSnapshot::set(SeriesKey key, Sample sample) {
  const auto at = std::ranges::lower_bound(values_, key, std::less<>{}, &Entry::first);
  if (at != values_.end() && at->first == key) {
    at->second = sample;
    return;
  }
  values_.emplace(at, std::move(key), sample);
}

std::optional<double> MetricsSnapshot::value(std::string_view metric,
                                             std::string_view entity) const {
  const SeriesKey key{.metric = std::string(metric), .entity = std::string(entity)};
  const auto at = std::ranges::lower_bound(values_, key, std::less<>{}, &Entry::first);
  if (at == values_.end() || at->first != key) {
    return std::nullopt;
  }
  return at->second.value;
}

std::vector<std::string> MetricsSnapshot::entities(std::string_view metric) const {
  std::vector<std::string> result;
  const SeriesKey first{.metric = std::string(metric), .entity = {}};
  for (auto it = std::ranges::lower_bound(values_, first, std::less<>{}, &Entry::first);
       it != values_.end() && it->first.metric == metric; ++it) {
    result.push_back(it->first.entity);
  }
  std::ranges::sort(result, natural_less);
  return result;
}

}  // namespace myclean::tray_model
