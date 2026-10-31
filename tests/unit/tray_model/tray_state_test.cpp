#include "core/tray_model/tray_state.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/series.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {
namespace {

constexpr double gib = 1024.0 * 1024.0 * 1024.0;

MetricsSnapshot metrics(double cpu, double available_gib, double commit_gib) {
  MetricsSnapshot snapshot;
  const auto set = [&snapshot](const char* metric, double value) {
    snapshot.set(SeriesKey{.metric = metric, .entity = "system"},
                 Sample{.time = {}, .value = value});
  };
  set("cpu.utility", cpu);
  set("memory.total", 64 * gib);
  set("memory.available", available_gib * gib);
  set("memory.commit", commit_gib * gib);
  set("memory.commit_limit", 100 * gib);
  return snapshot;
}

const MetricsSnapshot calm = metrics(20.0, 40.0, 30.0);

TEST(TrayStateTest, NormalWhenReachableCalmAndWithoutAlerts) {
  const TrayStatus status = tray_status(true, calm, std::nullopt);
  EXPECT_EQ(status.state, TrayState::normal);
  EXPECT_FALSE(status.badge);
  EXPECT_FALSE(status.severity);
}

TEST(TrayStateTest, ServiceDownWinsOverEverything) {
  const TrayStatus status = tray_status(
      false, metrics(99.0, 1.0, 99.0), AlertSummary{.active = 3, .worst = AlertSeverity::critical});
  EXPECT_EQ(status.state, TrayState::service_down);
  EXPECT_FALSE(status.badge);
}

TEST(TrayStateTest, PressureFromCpuCommitOrAvailableMemory) {
  EXPECT_EQ(tray_status(true, metrics(95.0, 40.0, 30.0), std::nullopt).state, TrayState::pressure);
  EXPECT_EQ(tray_status(true, metrics(20.0, 40.0, 91.0), std::nullopt).state, TrayState::pressure);
  EXPECT_EQ(tray_status(true, metrics(20.0, 6.0, 30.0), std::nullopt).state, TrayState::pressure);
  EXPECT_EQ(tray_status(true, metrics(89.0, 7.0, 89.0), std::nullopt).state, TrayState::normal);
}

TEST(TrayStateTest, NoPressureWithoutSamples) {
  EXPECT_FALSE(under_pressure(MetricsSnapshot{}, PressureLimits{}));
}

TEST(TrayStateTest, AnAlertTakesTheColorOfItsSeverityAndWinsOverPressure) {
  const MetricsSnapshot busy = metrics(99.0, 40.0, 30.0);
  const TrayStatus warning =
      tray_status(true, busy, AlertSummary{.active = 1, .worst = AlertSeverity::warning});
  EXPECT_EQ(warning.state, TrayState::alert);
  EXPECT_EQ(warning.severity, AlertSeverity::warning);
  EXPECT_TRUE(warning.badge);
  const TrayStatus critical =
      tray_status(true, calm, AlertSummary{.active = 2, .worst = AlertSeverity::critical});
  EXPECT_EQ(critical.state, TrayState::alert);
  EXPECT_NE(state_color(warning), state_color(critical));
}

TEST(TrayStateTest, InformationalAlertsShowOnlyTheBadge) {
  const TrayStatus status =
      tray_status(true, calm, AlertSummary{.active = 1, .worst = AlertSeverity::info});
  EXPECT_EQ(status.state, TrayState::normal);
  EXPECT_TRUE(status.badge);
}

TEST(TrayStateTest, NoActiveAlertMeansNoBadge) {
  EXPECT_FALSE(tray_status(true, calm, AlertSummary{.active = 0, .worst = std::nullopt}).badge);
}

TEST(TrayStateTest, EveryStateHasItsOwnColor) {
  const std::vector<TrayStatus> states = {
      {.state = TrayState::normal, .severity = std::nullopt, .badge = false},
      {.state = TrayState::pressure, .severity = std::nullopt, .badge = false},
      {.state = TrayState::alert, .severity = AlertSeverity::warning, .badge = true},
      {.state = TrayState::alert, .severity = AlertSeverity::critical, .badge = true},
      {.state = TrayState::service_down, .severity = std::nullopt, .badge = false}};
  for (std::size_t i = 0; i < states.size(); ++i) {
    EXPECT_EQ(state_color(states[i]) >> 24U, 0xFFU) << "opaque";
    for (std::size_t j = i + 1; j < states.size(); ++j) {
      EXPECT_NE(state_color(states[i]), state_color(states[j])) << i << " vs " << j;
    }
  }
}

TEST(TrayStateTest, IconIsADiscInTheStateColorWithTransparentCorners) {
  const TrayStatus status{.state = TrayState::pressure, .severity = std::nullopt, .badge = false};
  constexpr int size = 32;
  const std::vector<Argb> pixels = icon_pixels(status, size);
  ASSERT_EQ(pixels.size(), static_cast<std::size_t>(size * size));
  EXPECT_EQ(pixels[(16 * size) + 16], state_color(status));
  EXPECT_EQ(pixels[0], 0U);
  EXPECT_EQ(pixels[(size * size) - 1], 0U);
  // Outside the disc, where the badge would be.
  EXPECT_EQ(pixels[(3 * size) + 29], 0U);
}

TEST(TrayStateTest, BadgeIsAWhiteDotInTheTopRightCorner) {
  const TrayStatus status{
      .state = TrayState::alert, .severity = AlertSeverity::critical, .badge = true};
  constexpr int size = 32;
  const std::vector<Argb> pixels = icon_pixels(status, size);
  EXPECT_EQ(pixels[(7 * size) + 24], 0xFFFFFFFFU);
  EXPECT_EQ(pixels[(3 * size) + 29] >> 24U, 0xFFU) << "the badge's rim reaches past the disc";
  EXPECT_EQ(pixels[(24 * size) + 8], state_color(status));
}

TEST(TrayStateTest, IconOfNoSizeIsEmpty) {
  EXPECT_TRUE(icon_pixels(TrayStatus{}, 0).empty());
}

TEST(TrayStateTest, TooltipListsTheHeadlineValues) {
  const std::vector<VolumeRow> volumes = {
      VolumeRow{.name = "D:", .free = Bytes{1ULL << 40U}},
      VolumeRow{
          .name = "C:", .size = Bytes{952ULL << 30U}, .free = Bytes{84ULL << 30U}, .system = true}};
  const std::string text = tooltip_text(tooltip_data(true, calm, volumes));
  EXPECT_EQ(text,
            "MyClean\nCPU 20 %\nRAM 24.0 GB / 64.0 GB\nCommit 30.0 GB / 100 GB\n"
            "C: 84.0 GB free\nDisk health: n/a");
  EXPECT_LE(text.size(), 127U);
}

TEST(TrayStateTest, TooltipShowsMissingValuesAsNotAvailable) {
  const std::string text = tooltip_text(tooltip_data(true, MetricsSnapshot{}, {}));
  EXPECT_EQ(text,
            "MyClean\nCPU n/a\nRAM n/a / n/a\nCommit n/a / n/a\nC: n/a free\nDisk health: n/a");
}

TEST(TrayStateTest, TooltipNamesTheWorstDiskHealthWhenKnown) {
  TooltipData data = tooltip_data(true, calm, {});
  data.worst_disk_health = "Disk 3 warning";
  EXPECT_NE(tooltip_text(data).find("Disk health: Disk 3 warning"), std::string::npos);
}

TEST(TrayStateTest, WorstDriveHealth) {
  const auto drive = [](std::string disk, DriveStatus status) {
    DriveHealthRow row;
    row.disk = std::move(disk);
    row.status = status;
    return row;
  };
  EXPECT_FALSE(worst_drive_health({}));
  EXPECT_EQ(worst_drive_health({drive("0", DriveStatus::good), drive("1", DriveStatus::good)}),
            "good");
  // A drive that cannot be read does not hide the others' status.
  EXPECT_EQ(worst_drive_health({drive("0", DriveStatus::good), drive("7", DriveStatus::unknown)}),
            "good");
  EXPECT_EQ(worst_drive_health({drive("7", DriveStatus::unknown)}), "unknown");
  EXPECT_EQ(worst_drive_health({drive("0", DriveStatus::warning), drive("3", DriveStatus::failing),
                                drive("5", DriveStatus::failing)}),
            "Disk 3 failing");
}

TEST(TrayStateTest, TooltipSaysWhenTheServiceIsDown) {
  EXPECT_EQ(tooltip_text(tooltip_data(false, calm, {})),
            "MyClean: service not running\nLive data unavailable");
}

TEST(TrayStateTest, TooltipIsCutAtALineBoundary) {
  EXPECT_EQ(tooltip_text(tooltip_data(true, calm, {}), 20), "MyClean\nCPU 20 %");
}

}  // namespace
}  // namespace myclean::tray_model
