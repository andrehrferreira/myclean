#include "core/tray_model/replies.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/tray_model/snapshot.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::gib;
using testing::Json;

TEST(RepliesTest, LatestBecomesSeriesOfTheMetricNames) {
  const Result<LatestReply> latest = parse_metrics_latest(testing::latest_json(1000));
  ASSERT_TRUE(latest.has_value()) << latest.error().message;
  EXPECT_EQ(latest->time, from_unix_seconds(1000));
  EXPECT_EQ(latest->logical_processors, 4U);
  const MetricsSnapshot& metrics = latest->metrics;
  EXPECT_EQ(metrics.value("cpu.utility", "system"), 42.0);
  EXPECT_EQ(metrics.value("cpu.queue", "system"), 2.0);
  EXPECT_FALSE(metrics.value("cpu.logical_processors", "system"));
  EXPECT_EQ(metrics.value("memory.commit_limit", "system"), 72 * gib);
  EXPECT_EQ(metrics.value("process.count", "system"), 300.0);
  EXPECT_EQ(metrics.value("disk.read_bytes", "2"), 5.0e6);
  EXPECT_EQ(metrics.entities("disk.idle"), (std::vector<std::string>{"0", "2", "10"}));
  EXPECT_EQ(metrics.value("gpu.utilization", "luid_0x0_0xD1B5_phys_0"), 12.0);
  EXPECT_EQ(metrics.value("gpu.engine", "luid_0x0_0xD1B5_phys_0/3D"), 12.0);
  for (const auto& [key, sample] : metrics.values()) {
    EXPECT_EQ(sample.time, from_unix_seconds(1000)) << key.metric;
  }
}

TEST(RepliesTest, LatestVolumes) {
  const Result<LatestReply> latest = parse_metrics_latest(testing::latest_json(1000));
  ASSERT_TRUE(latest.has_value());
  ASSERT_EQ(latest->volumes.size(), 2U);
  const VolumeRow& c = latest->volumes[0];
  EXPECT_EQ(c.name, "C:");
  EXPECT_TRUE(c.system);
  EXPECT_EQ(c.free, Bytes{84ULL << 30U});
  EXPECT_EQ(c.fill, FillState::filling);
  EXPECT_EQ(c.days_until_full, 40.5);
  EXPECT_EQ(c.disks, std::vector<std::string>{"0"});
  const VolumeRow& d = latest->volumes[1];
  EXPECT_EQ(d.fill, FillState::not_filling);
  EXPECT_FALSE(d.days_until_full);
  EXPECT_EQ(d.disks, (std::vector<std::string>{"2", "10"}));
}

TEST(RepliesTest, LatestWithoutGpuOrSamplesYet) {
  Json reply = testing::latest_json(5);
  reply["gpu"] = nullptr;
  reply["cpu"] = Json{{"logical_processors", 4}};
  reply["disks"] = Json::array();
  reply["unavailable"] = Json::array({"gpu: no GPU performance counters"});
  const Result<LatestReply> latest = parse_metrics_latest(reply);
  ASSERT_TRUE(latest.has_value());
  EXPECT_FALSE(latest->metrics.value("cpu.utility", "system"));
  EXPECT_TRUE(latest->metrics.entities("gpu.utilization").empty());
  EXPECT_EQ(latest->unavailable, std::vector<std::string>{"gpu: no GPU performance counters"});
}

TEST(RepliesTest, MalformedLatestIsInvalidData) {
  Json reply = testing::latest_json(5);
  reply.erase("memory");
  const Result<LatestReply> latest = parse_metrics_latest(reply);
  ASSERT_FALSE(latest.has_value());
  EXPECT_EQ(latest.error().code, Errc::invalid_data);
  EXPECT_FALSE(parse_metrics_latest(Json("text")).has_value());
}

TEST(RepliesTest, QueryParametersUseTheEntityToken) {
  EXPECT_EQ(metrics_query_params("cpu.utility", "0,17", 100, 160),
            (Json{{"metric", "cpu.utility"}, {"entity", "0_17"}, {"from", 100}, {"to", 160}}));
}

TEST(RepliesTest, QueryPointsBecomeSamplesOfTheirMeans) {
  Json reply = testing::query_json("cpu.utility", "0,3", 100, 103, 7.0);
  reply["points"][1]["mean"] = 9.0;
  const Result<Series> series = parse_metrics_query(reply);
  ASSERT_TRUE(series.has_value());
  EXPECT_EQ(series->key, (SeriesKey{.metric = "cpu.utility", .entity = "0,3"}));
  ASSERT_EQ(series->samples.size(), 3U);
  EXPECT_EQ(series->samples[1].time, from_unix_seconds(101));
  EXPECT_EQ(series->samples[1].value, 9.0);
  EXPECT_FALSE(parse_metrics_query(Json{{"metric", "x"}}).has_value());
}

TEST(RepliesTest, TopParameters) {
  EXPECT_EQ(processes_top_params(ProcessSort::memory, 25),
            (Json{{"sort", "memory"}, {"limit", 25}}));
  EXPECT_EQ(processes_top_params(ProcessSort::cpu, 5), (Json{{"sort", "cpu"}, {"limit", 5}}));
}

TEST(RepliesTest, TopTreesWithMembers) {
  const Result<std::vector<ProcessTreeRow>> trees = parse_processes_top(testing::top_json());
  ASSERT_TRUE(trees.has_value()) << trees.error().message;
  ASSERT_EQ(trees->size(), 2U);
  const ProcessTreeRow& browser = (*trees)[0];
  EXPECT_EQ(browser.cpu_percent, 9.0);
  EXPECT_EQ(browser.private_working_set, Bytes{300ULL << 20U});
  ASSERT_EQ(browser.members.size(), 3U);
  EXPECT_EQ(browser.members[0].pid, 100U);
  EXPECT_EQ(browser.members[2].image, "gpu-helper.exe");
  EXPECT_EQ(browser.members[2].cpu_percent, 1.0);
}

TEST(RepliesTest, TreeWithoutMembersIsInvalid) {
  Json reply = testing::top_json();
  reply["trees"][0]["members"] = Json::array();
  EXPECT_FALSE(parse_processes_top(reply).has_value());
}

TEST(RepliesTest, ProcessorEntitiesFollowProcessorGroups) {
  EXPECT_EQ(processor_entity(0), "0,0");
  EXPECT_EQ(processor_entity(17), "0,17");
  EXPECT_EQ(processor_entity(64), "1,0");
}

TEST(RepliesTest, UnixSeconds) {
  EXPECT_EQ(unix_seconds(from_unix_seconds(1'700'000'000)), 1'700'000'000);
  EXPECT_EQ(unix_seconds(WallTime{std::chrono::milliseconds{2999}}), 2);
}

}  // namespace
}  // namespace myclean::tray_model
