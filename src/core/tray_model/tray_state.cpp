#include "core/tray_model/tray_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/telemetry/metrics.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

namespace metric = telemetry::metric;

namespace {

constexpr Argb rim_color = 0xFF1B1F24;
constexpr Argb badge_color = 0xFFFFFFFF;

// Straight-alpha "over" of `top` with coverage `alpha` onto `bottom`.
Argb blend(Argb bottom, Argb top, double alpha) {
  const auto channel = [](Argb color, unsigned shift) {
    return static_cast<double>((color >> shift) & 0xFFU) / 255.0;
  };
  const double top_a = channel(top, 24) * alpha;
  const double bottom_a = channel(bottom, 24);
  const double out_a = top_a + (bottom_a * (1.0 - top_a));
  if (out_a <= 0.0) {
    return 0;
  }
  Argb result = static_cast<Argb>(std::lround(out_a * 255.0)) << 24U;
  for (const unsigned shift : {16U, 8U, 0U}) {
    const double value =
        ((channel(top, shift) * top_a) + (channel(bottom, shift) * bottom_a * (1.0 - top_a))) /
        out_a;
    result |= static_cast<Argb>(std::lround(value * 255.0)) << shift;
  }
  return result;
}

// Paints a disc of radius `radius` centered at (cx, cy), anti-aliased by pixel coverage.
void paint_disc(std::vector<Argb>& pixels, int size, double cx, double cy, double radius,
                Argb color) {
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double distance = std::hypot((x + 0.5) - cx, (y + 0.5) - cy);
      const double coverage = std::clamp(radius - distance + 0.5, 0.0, 1.0);
      if (coverage > 0.0) {
        Argb& pixel = pixels.at((static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) +
                                static_cast<std::size_t>(x));
        pixel = blend(pixel, color, coverage);
      }
    }
  }
}

}  // namespace

bool under_pressure(const MetricsSnapshot& metrics, const PressureLimits& limits) {
  const std::optional<double> cpu = metrics.value(metric::cpu_utility, metric::system);
  if (cpu && *cpu >= limits.cpu_percent) {
    return true;
  }
  const std::optional<double> commit = metrics.value(metric::memory_commit, metric::system);
  const std::optional<double> limit = metrics.value(metric::memory_commit_limit, metric::system);
  if (commit && limit && *limit > 0.0 && *commit >= *limit * limits.commit_fraction) {
    return true;
  }
  const std::optional<double> available = metrics.value(metric::memory_available, metric::system);
  const std::optional<double> total = metrics.value(metric::memory_total, metric::system);
  return available && total && *total > 0.0 && *available < *total * limits.available_fraction;
}

TrayStatus tray_status(bool service_reachable, const MetricsSnapshot& metrics,
                       const std::optional<AlertSummary>& alerts, const PressureLimits& limits) {
  if (!service_reachable) {
    return TrayStatus{.state = TrayState::service_down, .severity = std::nullopt, .badge = false};
  }
  const bool badge = alerts && alerts->active > 0;
  if (badge && alerts->worst && *alerts->worst != AlertSeverity::info) {
    return TrayStatus{.state = TrayState::alert, .severity = alerts->worst, .badge = true};
  }
  const TrayState state = under_pressure(metrics, limits) ? TrayState::pressure : TrayState::normal;
  return TrayStatus{.state = state, .severity = std::nullopt, .badge = badge};
}

Argb state_color(const TrayStatus& status) {
  switch (status.state) {
    case TrayState::normal:
      return 0xFF2EB872;
    case TrayState::pressure:
      return 0xFFE8B21E;
    case TrayState::alert:
      return status.severity == AlertSeverity::critical ? 0xFFE5484D : 0xFFF07F2E;
    case TrayState::service_down:
      break;
  }
  return 0xFF8B939C;
}

std::vector<Argb> icon_pixels(const TrayStatus& status, int size) {
  std::vector<Argb> pixels(static_cast<std::size_t>(std::max(size, 0) * std::max(size, 0)), 0);
  if (size <= 0) {
    return pixels;
  }
  const double s = size;
  const double center = s / 2.0;
  const double rim = std::max(1.0, s / 16.0);
  paint_disc(pixels, size, center, center, (s / 2.0) - 0.5, rim_color);
  paint_disc(pixels, size, center, center, (s / 2.0) - 0.5 - rim, state_color(status));
  if (status.badge) {
    const double radius = s * 0.22;
    const double badge_center = s - radius - 0.5;
    paint_disc(pixels, size, badge_center, radius + 0.5, radius + 0.5, rim_color);
    paint_disc(pixels, size, badge_center, radius + 0.5, radius + 0.5 - rim, badge_color);
  }
  return pixels;
}

TooltipData tooltip_data(bool service_reachable, const MetricsSnapshot& metrics,
                         const std::vector<VolumeRow>& volumes) {
  TooltipData data;
  data.service_reachable = service_reachable;
  data.cpu_percent = metrics.value(metric::cpu_utility, metric::system);
  data.memory_total = metrics.value(metric::memory_total, metric::system);
  const std::optional<double> available = metrics.value(metric::memory_available, metric::system);
  if (data.memory_total && available) {
    data.memory_used = std::max(0.0, *data.memory_total - *available);
  }
  data.commit = metrics.value(metric::memory_commit, metric::system);
  data.commit_limit = metrics.value(metric::memory_commit_limit, metric::system);
  const auto system = std::ranges::find_if(volumes, &VolumeRow::system);
  if (system != volumes.end()) {
    data.system_volume = system->name;
    data.system_free = static_cast<double>(system->free.count());
  }
  return data;
}

std::string tooltip_text(const TooltipData& data, std::size_t max_chars) {
  std::vector<std::string> lines;
  if (!data.service_reachable) {
    lines = {"MyClean: service not running", "Live data unavailable"};
  } else {
    const auto of = [](const std::optional<double>& part, const std::optional<double>& whole) {
      return format_bytes(part) + " / " + format_bytes(whole);
    };
    lines = {"MyClean",
             "CPU " + format_percent(data.cpu_percent),
             "RAM " + of(data.memory_used, data.memory_total),
             "Commit " + of(data.commit, data.commit_limit),
             data.system_volume + " " + format_bytes(data.system_free) + " free",
             "Disk health: " + data.worst_disk_health.value_or(std::string(not_available))};
  }
  std::string text;
  for (const std::string& line : lines) {
    const std::size_t needed = text.size() + (text.empty() ? 0 : 1) + line.size();
    if (needed > max_chars) {
      break;
    }
    if (!text.empty()) {
      text += '\n';
    }
    text += line;
  }
  return text;
}

}  // namespace myclean::tray_model
