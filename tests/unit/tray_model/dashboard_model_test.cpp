#include "core/tray_model/dashboard_model.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/snapshot.hpp"
#include "core/tray_model/tray_state.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;

constexpr std::int64_t start = 1'000'000;

PollResult reachable_at(std::int64_t second, testing::Json reply = {}) {
  if (reply.is_null()) {
    reply = testing::latest_json(second);
  }
  Result<LatestReply> latest = parse_metrics_latest(reply);
  EXPECT_TRUE(latest.has_value());
  PollResult result;
  result.time = from_unix_seconds(second);
  result.reachable = true;
  result.latest = std::move(*latest);
  return result;
}

PollResult unreachable_at(std::int64_t second) {
  PollResult result;
  result.time = from_unix_seconds(second);
  result.error = R"(connect to \\.\pipe\myclean: the system cannot find the file specified)";
  return result;
}

TEST(DashboardModelTest, StartsDisconnectedWithoutData) {
  const DashboardModel model;
  EXPECT_FALSE(model.service_reachable());
  EXPECT_FALSE(model.metrics_time());
  EXPECT_EQ(model.tray_status().state, TrayState::service_down);
  EXPECT_EQ(model.tooltip(), "MyClean: service not running\nLive data unavailable");
}

TEST(DashboardModelTest, LatestMetricsFillTheChartsOncePerSecond) {
  DashboardModel model;
  model.apply(reachable_at(start));
  model.apply(reachable_at(start));  // a poll within the same sample adds nothing
  model.apply(reachable_at(start + 1));
  EXPECT_TRUE(model.service_reachable());
  EXPECT_EQ(model.metrics_time(), from_unix_seconds(start + 1));
  const ChartSeries* cpu = model.chart(SeriesKey{.metric = "cpu.utility", .entity = "system"});
  ASSERT_NE(cpu, nullptr);
  EXPECT_EQ(cpu->times().size(), 2U);
  EXPECT_EQ(model.disks(), (std::vector<std::string>{"0", "2", "10"}));
  EXPECT_EQ(model.logical_processors(), 4U);
  EXPECT_EQ(model.volumes().size(), 2U);
  EXPECT_EQ(model.tray_status().state, TrayState::normal);
  EXPECT_NE(model.tooltip().find("CPU 42 %"), std::string::npos);
  EXPECT_NE(model.tooltip().find("C: 84.0 GB free"), std::string::npos);
  EXPECT_TRUE(model.unavailable_features().empty());
}

TEST(DashboardModelTest, HistoryFillsTheChartsInTimeOrder) {
  DashboardModel model;
  model.apply(reachable_at(start));
  PollResult late = reachable_at(start + 3);
  late.history.push_back(
      Series{.key = SeriesKey{.metric = "cpu.utility", .entity = "system"},
             .samples = {Sample{.time = from_unix_seconds(start + 1), .value = 1.0},
                         Sample{.time = from_unix_seconds(start + 2), .value = 2.0}}});
  late.history.push_back(
      Series{.key = SeriesKey{.metric = "cpu.utility", .entity = "0,1"},
             .samples = {Sample{.time = from_unix_seconds(start + 2), .value = 70.0}}});
  model.apply(std::move(late));
  const ChartSeries* cpu = model.chart(SeriesKey{.metric = "cpu.utility", .entity = "system"});
  ASSERT_NE(cpu, nullptr);
  EXPECT_EQ(std::vector<double>(cpu->values().begin(), cpu->values().end()),
            (std::vector<double>{42.0, 1.0, 2.0, 42.0}));
  const ChartSeries* core = model.chart(SeriesKey{.metric = "cpu.utility", .entity = "0,1"});
  ASSERT_NE(core, nullptr);
  EXPECT_EQ(core->newest_value(), 70.0);
}

TEST(DashboardModelTest, ServiceStopKeepsTheLastValuesAndStopsTheCharts) {
  DashboardModel model;
  model.apply(reachable_at(start));
  model.apply(unreachable_at(start + 5));
  EXPECT_FALSE(model.service_reachable());
  EXPECT_NE(model.service_error().find("cannot find"), std::string::npos);
  EXPECT_EQ(model.metrics_time(), from_unix_seconds(start));
  EXPECT_EQ(model.metrics().value("cpu.utility", "system"), 42.0);
  EXPECT_EQ(model.chart(SeriesKey{.metric = "cpu.utility", .entity = "system"})->times().size(),
            1U);
  EXPECT_EQ(model.tray_status().state, TrayState::service_down);
  EXPECT_EQ(model.unavailable_features(),
            (std::vector<std::string>{"live telemetry", "history", "disk health", "alerts",
                                      "freezes", "cleanup", "space"}));
  model.apply(reachable_at(start + 6));
  EXPECT_TRUE(model.service_reachable());
  EXPECT_TRUE(model.service_error().empty());
}

TEST(DashboardModelTest, PressureShowsInTheTrayStatus) {
  testing::Json busy = testing::latest_json(start);
  busy["cpu"]["utility"] = 97.0;
  DashboardModel model;
  model.apply(reachable_at(start, busy));
  EXPECT_EQ(model.tray_status().state, TrayState::pressure);
}

TEST(DashboardModelTest, ASeriesThatStopsReportingLeavesWithItsWindow) {
  DashboardModel model(10s);
  model.apply(reachable_at(start));
  testing::Json without_disk_10 = testing::latest_json(start + 11);
  without_disk_10["disks"].erase(1);
  model.apply(reachable_at(start + 11, without_disk_10));
  EXPECT_EQ(model.chart(SeriesKey{.metric = "disk.idle", .entity = "10"}), nullptr);
  EXPECT_NE(model.chart(SeriesKey{.metric = "disk.idle", .entity = "2"}), nullptr);
  EXPECT_EQ(model.disks(), (std::vector<std::string>{"0", "2"}));
}

TEST(DashboardModelTest, VolumesPerDisk) {
  DashboardModel model;
  model.apply(reachable_at(start));
  EXPECT_EQ(model.volumes_on("0"), std::vector<std::string>{"C:"});
  EXPECT_EQ(model.volumes_on("10"), std::vector<std::string>{"D:"});
  EXPECT_TRUE(model.volumes_on("7").empty());
}

TEST(DashboardModelTest, TopTreesAreKeptUntilTheNextAnswer) {
  DashboardModel model;
  PollResult open = reachable_at(start);
  open.top_by_cpu = parse_processes_top(testing::top_json()).value();
  open.top_by_memory = std::vector<ProcessTreeRow>{};
  model.apply(std::move(open));
  model.apply(reachable_at(start + 1));  // the dashboard closed: no top trees asked
  const auto& by_cpu = model.top_by_cpu();
  ASSERT_TRUE(by_cpu.has_value());
  EXPECT_EQ((*by_cpu).size(), 2U);
  const auto& by_memory = model.top_by_memory();
  ASSERT_TRUE(by_memory.has_value());
  EXPECT_TRUE((*by_memory).empty());
}

TEST(DashboardModelTest, CollectorsUnavailableOnThisMachine) {
  testing::Json reply = testing::latest_json(start);
  reply["unavailable"] = testing::Json::array({"gpu: no GPU performance counters"});
  DashboardModel model;
  model.apply(reachable_at(start, reply));
  EXPECT_EQ(model.unavailable_collectors(),
            std::vector<std::string>{"gpu: no GPU performance counters"});
}

TEST(DashboardModelTest, ProcessorsAreChartedUnderTheirTokenEntity) {
  DashboardModel model;
  model.apply(reachable_at(start));
  EXPECT_EQ(model.processors(), (std::vector<std::string>{"0_0", "0_1", "0_2", "0_3"}));
  const ChartSeries* core = model.chart(SeriesKey{.metric = "cpu.utility", .entity = "0_2"});
  ASSERT_NE(core, nullptr);
  EXPECT_EQ(core->newest_value(), 30.0);
}

TEST(DashboardModelTest, TheBadgeColorsTheIcon) {
  DashboardModel model;
  PollResult critical = reachable_at(start);
  critical.badge = AlertSummary{.active = 2, .worst = AlertSeverity::critical};
  model.apply(std::move(critical));
  EXPECT_EQ(
      model.tray_status(),
      (TrayStatus{.state = TrayState::alert, .severity = AlertSeverity::critical, .badge = true}));
  // A service without alerts.badge: no alert state and no badge.
  model.apply(reachable_at(start + 1));
  EXPECT_FALSE(model.badge());
  EXPECT_EQ(model.tray_status().state, TrayState::normal);
  EXPECT_FALSE(model.tray_status().badge);
}

// A poll that carries `alerts`, each {rule, entity, toast_at or null}.
PollResult with_alerts(std::int64_t second, testing::Json alerts) {
  PollResult result = reachable_at(second);
  result.alerts = parse_alerts_list(testing::alerts_json(std::move(alerts))).value();
  return result;
}

TEST(DashboardModelTest, ToastsOnlyForToastsDecidedAfterTheTrayConnected) {
  DashboardModel model;
  // Raised while the tray was not running: badge only.
  model.apply(with_alerts(start, testing::Json::array({testing::alert_json(
                                     "volume.fill-up", "C", "warning", true, start - 100)})));
  EXPECT_TRUE(model.take_toasts().empty());
  ASSERT_TRUE(model.alerts().has_value());
  EXPECT_EQ((*model.alerts()).size(), 1U);
  // Unchanged: nothing new.
  model.apply(with_alerts(start + 1, testing::Json::array({testing::alert_json(
                                         "volume.fill-up", "C", "warning", true, start - 100)})));
  EXPECT_TRUE(model.take_toasts().empty());
  // Newest first, as alerts.list answers: a new alert on disk 3, a new decision for C:, an
  // alert without a toast decision (snoozed or within its rate limit).
  model.apply(with_alerts(
      start + 2,
      testing::Json::array(
          {testing::alert_json("disk.temperature", "disk3", "critical", true, start + 2),
           testing::alert_json("volume.fill-up", "C", "warning", true, start + 1),
           testing::alert_json("disk.temperature", "disk5", "warning", true, nullptr)})));
  const std::vector<AlertRow> toasts = model.take_toasts();
  ASSERT_EQ(toasts.size(), 2U);
  EXPECT_EQ(toasts[0].entity, "C") << "oldest first";
  EXPECT_EQ(toasts[1].entity, "disk3");
  EXPECT_EQ(toasts[1].severity, AlertSeverity::critical);
  EXPECT_TRUE(model.take_toasts().empty()) << "each toast is taken once";
}

TEST(DashboardModelTest, TheWorstDriveIsInTheTooltip) {
  DashboardModel model;
  PollResult result = reachable_at(start);
  result.drives = parse_disks_health(testing::health_json()).value();
  model.apply(std::move(result));
  EXPECT_NE(model.tooltip().find("Disk health: Disk 0 warning"), std::string::npos)
      << model.tooltip();
  ASSERT_NE(model.drive("7"), nullptr);
  EXPECT_EQ(model.drive("7")->model, "ROG ESD-S1C");
  EXPECT_EQ(model.drive("3"), nullptr);
  // Kept until the next reading (every minute).
  model.apply(reachable_at(start + 1));
  EXPECT_NE(model.drive("0"), nullptr);
}

TEST(DashboardModelTest, APastRangeReplacesThePreviousOne) {
  DashboardModel model;
  model.apply(reachable_at(start));
  EXPECT_FALSE(model.range());
  const SeriesKey cpu{.metric = "cpu.utility", .entity = "system"};
  const SeriesKey idle{.metric = "disk.idle", .entity = "0"};
  PollResult hour = reachable_at(start + 1);
  hour.range = RangeHistory{
      .range = Range::hour,
      .time = from_unix_seconds(start + 1),
      .series = {Series{.key = cpu,
                        .samples = {Sample{.time = from_unix_seconds(start - 3000), .value = 7.0},
                                    Sample{.time = from_unix_seconds(start - 10), .value = 8.0}}},
                 Series{.key = idle, .samples = {}}}};
  model.apply(std::move(hour));
  EXPECT_EQ(model.range(), Range::hour);
  EXPECT_EQ(model.range_time(), from_unix_seconds(start + 1));
  ASSERT_NE(model.range_chart(cpu), nullptr);
  EXPECT_EQ(model.range_chart(cpu)->values().size(), 2U);
  EXPECT_NE(model.range_chart(idle), nullptr);
  EXPECT_EQ(model.chart(cpu)->values().size(), 2U) << "the live chart is separate";

  PollResult day = reachable_at(start + 2);
  day.range = RangeHistory{.range = Range::day,
                           .time = from_unix_seconds(start + 2),
                           .series = {Series{.key = idle, .samples = {}}}};
  model.apply(std::move(day));
  EXPECT_EQ(model.range(), Range::day);
  EXPECT_EQ(model.range_chart(cpu), nullptr);
}

TEST(DashboardModelTest, ExplanationAndAlertActionsAreKeptUntilTheNextAnswer) {
  DashboardModel model;
  PollResult explained = reachable_at(start);
  explained.explanation = parse_processes_explain(testing::explain_json());
  explained.alert_action = "Snooze failed: no such alert";
  model.apply(std::move(explained));
  model.apply(reachable_at(start + 1));
  const auto& explanation = model.explanation();
  ASSERT_TRUE(explanation.has_value());
  ASSERT_TRUE((*explanation).has_value());
  EXPECT_EQ((*explanation).value().pid, 101U);
  EXPECT_EQ(model.alert_action_error(), "Snooze failed: no such alert");
  PollResult worked = reachable_at(start + 2);
  worked.alert_action = "";
  model.apply(std::move(worked));
  EXPECT_TRUE(model.alert_action_error().empty());
}

TEST(DashboardModelTest, FreezesAndTheIncidentQueue) {
  DashboardModel model;
  model.set_watchdog_error("RegisterRawInputDevices failed: Win32 error 5");
  EXPECT_EQ(model.watchdog_error(), "RegisterRawInputDevices failed: Win32 error 5");
  PollResult result = reachable_at(start);
  result.freezes = parse_input_summary(testing::summary_json()).value();
  model.apply(std::move(result));
  ASSERT_TRUE(model.freezes().has_value());
  EXPECT_EQ((*model.freezes()).size(), 2U);
  // The queue's counts change while the service is down too.
  PollResult down = unreachable_at(start + 1);
  down.incidents = IncidentQueue::Counts{.sent = 2, .dropped = 1, .rejected = 0, .queued = 4};
  model.apply(std::move(down));
  const auto& counts = model.incident_counts();
  ASSERT_TRUE(counts.has_value());
  EXPECT_EQ((*counts).queued, 4U);
  EXPECT_TRUE(model.freezes().has_value()) << "the last view is kept";
}

}  // namespace
}  // namespace myclean::tray_model
