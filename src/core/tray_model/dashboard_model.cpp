#include "core/tray_model/dashboard_model.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray_model {

void DashboardModel::apply(PollResult result) {
  reachable_ = result.reachable;
  error_ = std::move(result.error);
  if (!reachable_) {
    return;
  }
  if (result.latest) {
    LatestReply& latest = *result.latest;
    metrics_ = std::move(latest.metrics);
    metrics_time_ = latest.time;
    volumes_ = std::move(latest.volumes);
    logical_processors_ = latest.logical_processors;
    unavailable_ = std::move(latest.unavailable);
    for (const auto& [key, sample] : metrics_.values()) {
      add_sample(key, sample);
    }
  }
  for (const Series& series : result.history) {
    for (const Sample& sample : series.samples) {
      add_sample(series.key, sample);
    }
  }
  if (metrics_time_) {
    // A series that stopped reporting (a removed disk) leaves the dashboard with its window.
    const double cutoff =
        chart_time(*metrics_time_) - std::chrono::duration<double>(window_).count();
    std::erase_if(charts_, [cutoff](const auto& entry) {
      return entry.second.newest_time().value_or(cutoff) < cutoff;
    });
  }
  if (result.top_by_cpu) {
    top_by_cpu_ = std::move(result.top_by_cpu);
  }
  if (result.top_by_memory) {
    top_by_memory_ = std::move(result.top_by_memory);
  }
}

void DashboardModel::add_sample(const SeriesKey& key, Sample sample) {
  auto at = std::ranges::lower_bound(charts_, key, std::less<>{},
                                     &std::pair<SeriesKey, ChartSeries>::first);
  if (at == charts_.end() || at->first != key) {
    at = charts_.emplace(at, key, ChartSeries(window_));
  }
  at->second.add(sample);
}

const ChartSeries* DashboardModel::chart(const SeriesKey& key) const {
  const auto at = std::ranges::lower_bound(charts_, key, std::less<>{},
                                           &std::pair<SeriesKey, ChartSeries>::first);
  return at == charts_.end() || at->first != key ? nullptr : &at->second;
}

std::vector<std::string> DashboardModel::disks() const {
  return metrics_.entities(telemetry::metric::disk_idle);
}

std::vector<std::string> DashboardModel::volumes_on(const std::string& disk) const {
  std::vector<std::string> names;
  for (const VolumeRow& volume : volumes_) {
    if (std::ranges::find(volume.disks, disk) != volume.disks.end()) {
      names.push_back(volume.name);
    }
  }
  return names;
}

TrayStatus DashboardModel::tray_status() const {
  // The service reports no alerts yet (alerts.list arrives with the alert engine, T7).
  return tray_model::tray_status(reachable_, metrics_, std::nullopt);
}

std::string DashboardModel::tooltip() const {
  return tooltip_text(tooltip_data(reachable_, metrics_, volumes_));
}

std::vector<std::string> DashboardModel::unavailable_features() const {
  if (reachable_) {
    return {};
  }
  return {"live telemetry", "history", "disk health", "alerts"};
}

}  // namespace myclean::tray_model
