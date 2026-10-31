#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <vector>

#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean::tray_model {

// Seconds since the Unix epoch, the x axis of every chart.
[[nodiscard]] double chart_time(WallTime time);

// The points of one live chart: samples ordered by time, one per instant, covering at most
// `window` before the newest. Times and values are separate arrays, as the plotting library
// takes them.
class ChartSeries {
 public:
  explicit ChartSeries(std::chrono::seconds window) : window_(window) {}

  // Inserts the sample in time order; a sample at an instant already present, or older than
  // the window, is ignored. Returns whether it was added.
  bool add(Sample sample);

  [[nodiscard]] std::span<const double> times() const { return times_; }
  [[nodiscard]] std::span<const double> values() const { return values_; }
  [[nodiscard]] bool empty() const { return times_.empty(); }
  [[nodiscard]] std::optional<double> newest_time() const;
  [[nodiscard]] std::optional<double> newest_value() const;

 private:
  std::chrono::seconds window_;
  std::vector<double> times_;
  std::vector<double> values_;
};

}  // namespace myclean::tray_model
