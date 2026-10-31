#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray_model {

// How far back the live charts reach.
inline constexpr std::chrono::seconds live_window{600};

// Everything the tray icon and the dashboard show, built from poll results. The poll thread
// applies results; the UI thread reads. Not thread-safe: the tray guards it with a mutex.
class DashboardModel {
 public:
  explicit DashboardModel(std::chrono::seconds window = live_window) : window_(window) {}

  // Takes in one poll. While the service is unreachable the last values are kept (the
  // dashboard shows them with their time) and the charts stop advancing.
  void apply(PollResult result);

  [[nodiscard]] std::chrono::seconds window() const { return window_; }
  [[nodiscard]] bool service_reachable() const { return reachable_; }
  // Why the service is unreachable; empty while it is reachable.
  [[nodiscard]] const std::string& service_error() const { return error_; }
  // The service's time of the newest metrics; nullopt before the first.
  [[nodiscard]] std::optional<WallTime> metrics_time() const { return metrics_time_; }
  [[nodiscard]] const MetricsSnapshot& metrics() const { return metrics_; }
  [[nodiscard]] const std::vector<VolumeRow>& volumes() const { return volumes_; }
  [[nodiscard]] unsigned logical_processors() const { return logical_processors_; }
  // Collectors the service cannot run on this machine, with the reason.
  [[nodiscard]] const std::vector<std::string>& unavailable_collectors() const {
    return unavailable_;
  }
  // The live chart of `key`; null when there is none.
  [[nodiscard]] const ChartSeries* chart(const SeriesKey& key) const;
  [[nodiscard]] const std::optional<std::vector<ProcessTreeRow>>& top_by_cpu() const {
    return top_by_cpu_;
  }
  [[nodiscard]] const std::optional<std::vector<ProcessTreeRow>>& top_by_memory() const {
    return top_by_memory_;
  }
  // The physical disks with a sample, in natural order of their numbers.
  [[nodiscard]] std::vector<std::string> disks() const;
  // The mount names of the volumes on physical disk `disk` ("C:", "D:").
  [[nodiscard]] std::vector<std::string> volumes_on(const std::string& disk) const;

  [[nodiscard]] TrayStatus tray_status() const;
  [[nodiscard]] std::string tooltip() const;
  // The features the dashboard cannot show live while the service is unreachable; empty while
  // it is reachable.
  [[nodiscard]] std::vector<std::string> unavailable_features() const;

 private:
  void add_sample(const SeriesKey& key, Sample sample);

  std::chrono::seconds window_;
  bool reachable_ = false;
  std::string error_ = "not connected yet";
  std::optional<WallTime> metrics_time_;
  MetricsSnapshot metrics_;
  std::vector<VolumeRow> volumes_;
  unsigned logical_processors_ = 0;
  std::vector<std::string> unavailable_;
  // Sorted by key; vectors, so that moving the model never throws.
  std::vector<std::pair<SeriesKey, ChartSeries>> charts_;
  std::optional<std::vector<ProcessTreeRow>> top_by_cpu_;
  std::optional<std::vector<ProcessTreeRow>> top_by_memory_;
};

}  // namespace myclean::tray_model
