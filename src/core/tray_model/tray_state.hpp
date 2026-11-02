#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/model/units.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

// What the tray icon shows, most important first: the service is down, an alert of warning or
// critical severity is active, the machine is under pressure, or all is normal.
enum class TrayState { normal, pressure, alert, service_down };

// The system is under pressure when any of these holds in the newest sample.
struct PressureLimits {
  double cpu_percent = 90.0;         // cpu.utility of the whole system at or above
  double commit_fraction = 0.90;     // commit charge at or above this share of the commit limit
  double available_fraction = 0.10;  // available memory below this share of physical memory
};

[[nodiscard]] bool under_pressure(const MetricsSnapshot& metrics, const PressureLimits& limits);

struct TrayStatus {
  TrayState state = TrayState::service_down;
  std::optional<AlertSeverity> severity;  // set when state is alert
  bool badge = false;                     // at least one active alert, of any severity

  friend bool operator==(const TrayStatus&, const TrayStatus&) = default;
};

// The icon's state. `alerts` is nullopt while the service does not report alerts.
[[nodiscard]] TrayStatus tray_status(bool service_reachable, const MetricsSnapshot& metrics,
                                     const std::optional<AlertSummary>& alerts,
                                     const PressureLimits& limits = {});

// 0xAARRGGBB, not premultiplied.
using Argb = std::uint32_t;

// The fill color of the icon: green (normal), amber (pressure), orange (warning alert), red
// (critical alert), gray (service down).
[[nodiscard]] Argb state_color(const TrayStatus& status);

// The pixels of a square icon of `size` pixels, row by row from the top: an anti-aliased disc
// in the state color with a dark rim, and, with a badge, a white dot with a dark rim in the
// top-right corner. Fully transparent outside the shapes. With `base` (the application icon,
// size x size pixels) the state disc is a dot in its bottom-right corner instead.
[[nodiscard]] std::vector<Argb> icon_pixels(const TrayStatus& status, int size,
                                            std::span<const Argb> base = {});

// What the tooltip reports; nullopt fields print as "n/a".
struct TooltipData {
  bool service_reachable = false;
  std::optional<double> cpu_percent;
  std::optional<double> memory_used;             // bytes: total - available
  std::optional<double> memory_total;            // bytes
  std::optional<double> commit;                  // bytes
  std::optional<double> commit_limit;            // bytes
  std::string system_volume = "C:";              // the volume Windows runs from
  std::optional<double> system_free;             // bytes free on it
  std::optional<std::string> worst_disk_health;  // worst_drive_health()
  int pending_approvals = 0;                     // plans waiting for the owner
};

// The tooltip's disk-health text: "good" when every drive is good, "Disk 3 warning" naming the
// first drive of the worst status otherwise, "unknown" when no drive could be read; nullopt
// without drives. A drive that cannot be read does not hide the others' status.
[[nodiscard]] std::optional<std::string> worst_drive_health(
    const std::vector<DriveHealthRow>& drives);

[[nodiscard]] TooltipData tooltip_data(bool service_reachable, const MetricsSnapshot& metrics,
                                       const std::vector<VolumeRow>& volumes);

// The tooltip text, lines separated by '\n'. At most `max_chars` UTF-8 bytes (the shell keeps
// 127 characters; the text is ASCII), cut at a line boundary.
[[nodiscard]] std::string tooltip_text(const TooltipData& data, std::size_t max_chars = 127);

}  // namespace myclean::tray_model
