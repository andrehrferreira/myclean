#include "core/tray_model/dashboard_model.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "core/model/series.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray_model {

namespace {

// The first chart of `charts` (sorted by key) whose key is not less than `key`.
template <typename Charts>
auto find_chart(Charts& charts, const SeriesKey& key) {
  return std::ranges::lower_bound(charts, key, std::less<>{},
                                  &std::pair<SeriesKey, ChartSeries>::first);
}

}  // namespace

void DashboardModel::apply(PollResult result) {
  reachable_ = result.reachable;
  error_ = std::move(result.error);
  if (result.incidents) {
    incident_counts_ = result.incidents;
  }
  if (!reachable_) {
    return;
  }
  if (result.latest) {
    LatestReply& latest = *result.latest;
    metrics_ = std::move(latest.metrics);
    metrics_time_ = latest.time;
    volumes_ = std::move(latest.volumes);
    logical_processors_ = latest.logical_processors;
    processors_ = std::move(latest.processors);
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
  if (result.range) {
    range_ = result.range->range;
    range_time_ = result.range->time;
    range_charts_.clear();
    const std::chrono::seconds length = range_length(result.range->range, window_);
    for (Series& series : result.range->series) {
      ChartSeries chart(length);
      for (const Sample& sample : series.samples) {
        chart.add(sample);
      }
      const auto at = find_chart(range_charts_, series.key);
      range_charts_.emplace(at, std::move(series.key), std::move(chart));
    }
  }
  if (result.top_by_cpu) {
    top_by_cpu_ = std::move(result.top_by_cpu);
  }
  if (result.top_by_memory) {
    top_by_memory_ = std::move(result.top_by_memory);
  }
  if (result.explanation) {
    explanation_ = std::move(result.explanation);
  }
  if (result.drives) {
    drives_ = std::move(result.drives);
  }
  // A service without alerts.badge reports no alerts: the icon then shows no badge.
  badge_ = result.badge;
  if (result.alert_action) {
    alert_action_error_ = std::move(*result.alert_action);
  }
  if (result.alerts) {
    take_alerts(std::move(*result.alerts));
  }
  if (result.freezes) {
    freezes_ = std::move(result.freezes);
  }
}

void DashboardModel::take_alerts(std::vector<AlertRow> alerts) {
  // Oldest first, so that toasts appear in the order the alerts fired.
  for (const AlertRow& alert : std::ranges::reverse_view(alerts)) {
    if (!alert.toast_at) {
      continue;
    }
    const auto seen = std::ranges::find_if(toasted_, [&](const Toasted& toasted) {
      return toasted.rule == alert.rule && toasted.entity == alert.entity;
    });
    if (seen == toasted_.end()) {
      toasted_.push_back(
          Toasted{.rule = alert.rule, .entity = alert.entity, .at = *alert.toast_at});
    } else if (*alert.toast_at > seen->at) {
      seen->at = *alert.toast_at;
    } else {
      continue;
    }
    if (toast_baseline_) {
      toasts_.push_back(alert);
    }
  }
  toast_baseline_ = true;
  alerts_ = std::move(alerts);
}

std::vector<AlertRow> DashboardModel::take_toasts() {
  return std::exchange(toasts_, {});
}

void DashboardModel::add_sample(const SeriesKey& key, Sample sample) {
  auto at = find_chart(charts_, key);
  if (at == charts_.end() || at->first != key) {
    at = charts_.emplace(at, key, ChartSeries(window_));
  }
  at->second.add(sample);
}

const ChartSeries* DashboardModel::chart(const SeriesKey& key) const {
  const auto at = find_chart(charts_, key);
  return at == charts_.end() || at->first != key ? nullptr : &at->second;
}

const ChartSeries* DashboardModel::range_chart(const SeriesKey& key) const {
  const auto at = find_chart(range_charts_, key);
  return at == range_charts_.end() || at->first != key ? nullptr : &at->second;
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

const DriveHealthRow* DashboardModel::drive(const std::string& disk) const {
  if (!drives_) {
    return nullptr;
  }
  const auto found = std::ranges::find(*drives_, disk, &DriveHealthRow::disk);
  return found == drives_->end() ? nullptr : &*found;
}

TrayStatus DashboardModel::tray_status() const {
  return tray_model::tray_status(reachable_, metrics_, badge_);
}

std::string DashboardModel::tooltip() const {
  TooltipData data = tooltip_data(reachable_, metrics_, volumes_);
  if (drives_) {
    data.worst_disk_health = worst_drive_health(*drives_);
  }
  return tooltip_text(data);
}

std::vector<std::string> DashboardModel::unavailable_features() const {
  if (reachable_) {
    return {};
  }
  return {"live telemetry", "history", "disk health", "alerts", "freezes"};
}

}  // namespace myclean::tray_model
