#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/incident_queue.hpp"
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
  // The cpu.* entities of the logical processors, in order.
  [[nodiscard]] const std::vector<std::string>& processors() const { return processors_; }
  // Collectors the service cannot run on this machine, with the reason.
  [[nodiscard]] const std::vector<std::string>& unavailable_collectors() const {
    return unavailable_;
  }
  // The live chart of `key`; null when there is none.
  [[nodiscard]] const ChartSeries* chart(const SeriesKey& key) const;
  // The past range whose charts the model holds, and when they were fetched; nullopt before
  // the first.
  [[nodiscard]] std::optional<Range> range() const { return range_; }
  [[nodiscard]] std::optional<WallTime> range_time() const { return range_time_; }
  // The chart of `key` over range(); null when there is none.
  [[nodiscard]] const ChartSeries* range_chart(const SeriesKey& key) const;
  [[nodiscard]] const std::optional<std::vector<ProcessTreeRow>>& top_by_cpu() const {
    return top_by_cpu_;
  }
  [[nodiscard]] const std::optional<std::vector<ProcessTreeRow>>& top_by_memory() const {
    return top_by_memory_;
  }
  // The answer to the latest Explain; nullopt before the first.
  [[nodiscard]] const std::optional<Result<Explanation>>& explanation() const {
    return explanation_;
  }
  // The physical disks with a sample, in natural order of their numbers.
  [[nodiscard]] std::vector<std::string> disks() const;
  // The mount names of the volumes on physical disk `disk` ("C:", "D:").
  [[nodiscard]] std::vector<std::string> volumes_on(const std::string& disk) const;
  // The newest health reading of every drive; nullopt before the first (or while the service
  // does not offer disk health).
  [[nodiscard]] const std::optional<std::vector<DriveHealthRow>>& drives() const { return drives_; }
  // The health reading of physical disk `disk`; null when there is none.
  [[nodiscard]] const DriveHealthRow* drive(const std::string& disk) const;

  // The badge counts; nullopt while the service does not report alerts.
  [[nodiscard]] const std::optional<AlertSummary>& badge() const { return badge_; }
  // The alerts of the last Poller::recent_alerts, newest first.
  [[nodiscard]] const std::optional<std::vector<AlertRow>>& alerts() const { return alerts_; }
  // Why the owner's last snooze or dismiss failed; empty when it worked or none was made.
  [[nodiscard]] const std::string& alert_action_error() const { return alert_action_error_; }
  // The alerts the service decided to toast since the previous call, oldest first. The first
  // alert list after the tray started only sets the baseline: alerts raised while the tray was
  // not running are reflected in the badge, not toasted.
  [[nodiscard]] std::vector<AlertRow> take_toasts();

  // The Freezes view; nullopt before the first.
  [[nodiscard]] const std::optional<std::vector<FreezeGroupRow>>& freezes() const {
    return freezes_;
  }
  [[nodiscard]] const std::optional<IncidentQueue::Counts>& incident_counts() const {
    return incident_counts_;
  }
  // Why the freeze watchdog is not running; empty while it runs. Set by the tray.
  void set_watchdog_error(std::string error) { watchdog_error_ = std::move(error); }
  [[nodiscard]] const std::string& watchdog_error() const { return watchdog_error_; }

  // The approval inbox, the Space view and the user-task runner.
  [[nodiscard]] const CleanupView& cleanup() const { return cleanup_; }

  // The icon's state; its badge also shows while a plan waits for the owner's approval.
  [[nodiscard]] TrayStatus tray_status() const;
  [[nodiscard]] std::string tooltip() const;
  // The features the dashboard cannot show live while the service is unreachable; empty while
  // it is reachable.
  [[nodiscard]] std::vector<std::string> unavailable_features() const;

 private:
  void add_sample(const SeriesKey& key, Sample sample);
  void take_alerts(std::vector<AlertRow> alerts);

  using Charts = std::vector<std::pair<SeriesKey, ChartSeries>>;  // sorted by key

  std::chrono::seconds window_;
  bool reachable_ = false;
  std::string error_ = "not connected yet";
  std::optional<WallTime> metrics_time_;
  MetricsSnapshot metrics_;
  std::vector<VolumeRow> volumes_;
  unsigned logical_processors_ = 0;
  std::vector<std::string> processors_;
  std::vector<std::string> unavailable_;
  // Vectors, so that moving the model never throws.
  Charts charts_;
  std::optional<Range> range_;
  std::optional<WallTime> range_time_;
  Charts range_charts_;
  std::optional<std::vector<ProcessTreeRow>> top_by_cpu_;
  std::optional<std::vector<ProcessTreeRow>> top_by_memory_;
  std::optional<Result<Explanation>> explanation_;
  std::optional<std::vector<DriveHealthRow>> drives_;
  std::optional<AlertSummary> badge_;
  std::optional<std::vector<AlertRow>> alerts_;
  std::string alert_action_error_;
  bool toast_baseline_ = false;
  // The newest toast time seen per alert.
  struct Toasted {
    std::string rule;
    std::string entity;
    WallTime at;
  };
  std::vector<Toasted> toasted_;
  std::vector<AlertRow> toasts_;
  std::optional<std::vector<FreezeGroupRow>> freezes_;
  std::optional<IncidentQueue::Counts> incident_counts_;
  std::string watchdog_error_;
  CleanupView cleanup_;
};

}  // namespace myclean::tray_model
