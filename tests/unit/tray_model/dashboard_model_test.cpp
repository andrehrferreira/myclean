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
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
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
            (std::vector<std::string>{"live telemetry", "history", "disk health", "alerts"}));
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

}  // namespace
}  // namespace myclean::tray_model
