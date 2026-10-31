#include "core/tray_model/chart.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <optional>

namespace myclean::tray_model {

double chart_time(WallTime time) {
  return std::chrono::duration<double>(time.time_since_epoch()).count();
}

bool ChartSeries::add(Sample sample) {
  const double time = chart_time(sample.time);
  const double window = std::chrono::duration<double>(window_).count();
  if (!times_.empty() && time < times_.back() - window) {
    return false;
  }
  const auto at = std::ranges::lower_bound(times_, time);
  if (at != times_.end() && *at == time) {
    return false;
  }
  const auto index = std::distance(times_.begin(), at);
  times_.insert(at, time);
  values_.insert(values_.begin() + index, sample.value);
  // Drop what fell out of the window behind the newest point.
  const auto keep = std::ranges::lower_bound(times_, times_.back() - window);
  const auto dropped = std::distance(times_.begin(), keep);
  if (dropped > 0) {
    times_.erase(times_.begin(), keep);
    values_.erase(values_.begin(), values_.begin() + dropped);
  }
  return true;
}

std::optional<double> ChartSeries::newest_time() const {
  return times_.empty() ? std::nullopt : std::optional<double>(times_.back());
}

std::optional<double> ChartSeries::newest_value() const {
  return values_.empty() ? std::nullopt : std::optional<double>(values_.back());
}

}  // namespace myclean::tray_model
