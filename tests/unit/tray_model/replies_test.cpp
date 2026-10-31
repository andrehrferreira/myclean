#include "core/tray_model/replies.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
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

TEST(RepliesTest, TopTreesCarryTheTrustOfTheirRoot) {
  Json reply = testing::top_json();
  reply["trees"].push_back(reply["trees"][1]);
  reply["trees"][2]["trust"] = nullptr;  // an image that could not be read (System)
  const Result<std::vector<ProcessTreeRow>> trees = parse_processes_top(reply);
  ASSERT_TRUE(trees.has_value()) << trees.error().message;
  ASSERT_TRUE((*trees)[0].trust.has_value());
  const TrustRow& known = *(*trees)[0].trust;
  EXPECT_EQ(known.status, "signed");
  EXPECT_EQ(known.signer, "Browser Vendor Inc.");
  EXPECT_FALSE(known.user_writable);
  EXPECT_FALSE(known.unrecognized);
  EXPECT_EQ(known.description, "Web browser");
  ASSERT_TRUE((*trees)[1].trust.has_value());
  const TrustRow& unknown = *(*trees)[1].trust;
  EXPECT_EQ(unknown.status, "unsigned");
  EXPECT_FALSE(unknown.signer);
  EXPECT_TRUE(unknown.user_writable);
  EXPECT_TRUE(unknown.unrecognized);
  EXPECT_EQ(unknown.reasons,
            (std::vector<std::string>{"not-in-knowledge-base", "unsigned", "user-writable-path"}));
  EXPECT_FALSE(unknown.description);
  EXPECT_FALSE((*trees)[2].trust);

  reply["trees"][0]["trust"]["status"] = "dangerous";
  EXPECT_FALSE(parse_processes_top(reply).has_value());
}

TEST(RepliesTest, LatestProcessorsInTokenForm) {
  const Result<LatestReply> latest = parse_metrics_latest(testing::latest_json(1000));
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->processors, (std::vector<std::string>{"0_0", "0_1", "0_2", "0_3"}));
  EXPECT_EQ(latest->metrics.value("cpu.utility", "0_2"), 30.0);
  EXPECT_EQ(latest->metrics.value("cpu.frequency", "0_3"), 70.0);
  EXPECT_FALSE(latest->metrics.value("cpu.entity", "0_0"));
}

TEST(RepliesTest, QueryParametersWithMaxPoints) {
  EXPECT_EQ(metrics_query_params("disk.idle", "3", 100, 160, 600), (Json{{"metric", "disk.idle"},
                                                                         {"entity", "3"},
                                                                         {"from", 100},
                                                                         {"to", 160},
                                                                         {"max_points", 600}}));
}

TEST(RepliesTest, Explanation) {
  EXPECT_EQ(processes_explain_params(101), (Json{{"pid", 101}}));
  const Result<Explanation> explanation = parse_processes_explain(testing::explain_json());
  ASSERT_TRUE(explanation.has_value()) << explanation.error().message;
  EXPECT_EQ(explanation->pid, 101U);
  EXPECT_EQ(explanation->tree_pid, 100U);
  EXPECT_EQ(explanation->tree_image, "browser.exe");
  EXPECT_EQ(explanation->members.size(), 3U);
  EXPECT_EQ(explanation->path, R"(C:\Program Files\Browser\browser.exe)");
  ASSERT_TRUE(explanation->trust.has_value());
  ASSERT_TRUE(explanation->kb.has_value());
  EXPECT_EQ((*explanation->kb).category, "browser");
  EXPECT_FALSE((*explanation->kb).safe_to_remove);

  Json unknown = testing::explain_json();
  unknown["kb"] = nullptr;
  unknown["trust"] = nullptr;
  unknown["path"] = nullptr;
  const Result<Explanation> bare = parse_processes_explain(unknown);
  ASSERT_TRUE(bare.has_value());
  EXPECT_FALSE(bare->kb);
  EXPECT_FALSE(bare->trust);
  EXPECT_FALSE(bare->path);
  unknown["tree"] = nullptr;  // a process in no tree
  unknown["members"] = Json::array();
  const Result<Explanation> alone = parse_processes_explain(unknown);
  ASSERT_TRUE(alone.has_value()) << alone.error().message;
  EXPECT_FALSE(alone->tree_pid);
  unknown.erase("tree");
  EXPECT_FALSE(parse_processes_explain(unknown).has_value());
}

TEST(RepliesTest, DiskHealth) {
  const Result<std::vector<DriveHealthRow>> drives = parse_disks_health(testing::health_json());
  ASSERT_TRUE(drives.has_value()) << drives.error().message;
  ASSERT_EQ(drives->size(), 3U);
  const DriveHealthRow& nvme = (*drives)[0];
  EXPECT_EQ(nvme.disk, "0");
  EXPECT_EQ(nvme.model, "Samsung SSD 980 PRO 2TB");
  EXPECT_EQ(nvme.status, DriveStatus::warning);
  EXPECT_EQ(nvme.reasons.size(), 2U);
  EXPECT_EQ(nvme.temperature_c, 74);
  EXPECT_EQ(nvme.temperature_limit_c, 70);
  EXPECT_EQ(nvme.media_errors, 1);
  EXPECT_FALSE(nvme.reallocated_sectors);
  EXPECT_EQ(nvme.data_written_bytes, 300'000'000'000'000LL);
  EXPECT_EQ(nvme.endurance_state, "projected");
  EXPECT_EQ(nvme.years_left, 18.0);
  EXPECT_EQ(nvme.tbw_rated, 1200.0);
  EXPECT_EQ(nvme.volumes, std::vector<std::string>{"C:"});
  const DriveHealthRow& usb = (*drives)[2];
  EXPECT_EQ(usb.status, DriveStatus::unknown);
  EXPECT_FALSE(usb.readable);
  ASSERT_TRUE(usb.unreadable_reason.has_value());
  EXPECT_TRUE((*usb.unreadable_reason).starts_with("not readable over USB"));
  EXPECT_EQ(usb.source, "reliability_counters");
  EXPECT_EQ(usb.temperature_c, 41);
  EXPECT_FALSE(usb.years_left);
  EXPECT_FALSE(usb.endurance_note.empty());

  Json failing = testing::health_json();
  failing["drives"][1]["status"] = "failing";
  EXPECT_EQ(parse_disks_health(failing).value()[1].status, DriveStatus::failing);
  failing["drives"][1]["status"] = "critical";
  EXPECT_FALSE(parse_disks_health(failing).has_value());
}

TEST(RepliesTest, AlertBadge) {
  const Result<AlertSummary> none = parse_alerts_badge(testing::badge_json(0, "none"));
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, (AlertSummary{.active = 0, .worst = std::nullopt}));
  EXPECT_EQ(parse_alerts_badge(testing::badge_json(2, "critical")).value(),
            (AlertSummary{.active = 2, .worst = AlertSeverity::critical}));
  EXPECT_FALSE(parse_alerts_badge(testing::badge_json(1, "severe")).has_value());
}

TEST(RepliesTest, AlertList) {
  Json reply = testing::alerts_json(Json::array(
      {testing::alert_json("volume.fill-up", "C", "warning", true, 940),
       testing::alert_json("disk.media-errors", "Sam_1A2B", "critical", false, nullptr)}));
  reply["alerts"][1]["snoozed_until"] = 5000;
  const Result<std::vector<AlertRow>> alerts = parse_alerts_list(reply);
  ASSERT_TRUE(alerts.has_value()) << alerts.error().message;
  ASSERT_EQ(alerts->size(), 2U);
  const AlertRow& fill = (*alerts)[0];
  EXPECT_EQ(fill.rule, "volume.fill-up");
  EXPECT_EQ(fill.entity, "C");
  EXPECT_EQ(fill.severity, AlertSeverity::warning);
  EXPECT_EQ(fill.first, from_unix_seconds(900));
  EXPECT_EQ(fill.last, from_unix_seconds(950));
  EXPECT_EQ(fill.count, 3);
  EXPECT_TRUE(fill.active);
  EXPECT_EQ(fill.toast_at, from_unix_seconds(940));
  EXPECT_FALSE(fill.snoozed_until);
  EXPECT_EQ(fill.detail, "C detail");
  EXPECT_EQ((*alerts)[1].snoozed_until, from_unix_seconds(5000));
  EXPECT_FALSE((*alerts)[1].toast_at);
  reply["alerts"][0].erase("toast_at");
  EXPECT_FALSE(parse_alerts_list(reply).has_value());
}

TEST(RepliesTest, AlertParameters) {
  EXPECT_EQ(alerts_list_params(from_unix_seconds(1234)), (Json{{"since", 1234}}));
  EXPECT_EQ(alerts_snooze_params("volume.fill-up", "C", std::chrono::days{1}),
            (Json{{"rule", "volume.fill-up"}, {"entity", "C"}, {"minutes", 1440}}));
  EXPECT_EQ(alerts_dismiss_params("volume.fill-up", "C"),
            (Json{{"rule", "volume.fill-up"}, {"entity", "C"}}));
}

TEST(RepliesTest, FreezeSummary) {
  const Result<std::vector<FreezeGroupRow>> groups = parse_input_summary(testing::summary_json());
  ASSERT_TRUE(groups.has_value()) << groups.error().message;
  ASSERT_EQ(groups->size(), 2U);
  const FreezeGroupRow& held = (*groups)[0];
  EXPECT_EQ(held.mechanism, "clipboard_held_open");
  EXPECT_EQ(held.process, "clipmgr.exe");
  EXPECT_EQ(held.count, 2);
  EXPECT_EQ(held.latest, from_unix_seconds(999));
  ASSERT_EQ(held.incidents.size(), 2U);
  EXPECT_EQ(held.incidents[0].id, 1);
  EXPECT_EQ(held.incidents[0].chord, "paste");
  EXPECT_EQ(held.incidents[0].latency_ms, 600);
  EXPECT_EQ(held.incidents[0].cause, "clipmgr.exe");
  EXPECT_EQ(held.incidents[0].evidence, std::vector<std::string>{"evidence of incident 1"});
  const FreezeGroupRow& unexplained = (*groups)[1];
  EXPECT_FALSE(unexplained.process);
  EXPECT_FALSE(unexplained.incidents[0].cause);
  EXPECT_TRUE(parse_input_summary(Json{{"groups", Json::array()}}).value().empty());
  EXPECT_FALSE(parse_input_summary(Json{{"groups", 3}}).has_value());
}

TEST(RepliesTest, UnixSeconds) {
  EXPECT_EQ(unix_seconds(from_unix_seconds(1'700'000'000)), 1'700'000'000);
  EXPECT_EQ(unix_seconds(WallTime{std::chrono::milliseconds{2999}}), 2);
}

}  // namespace
}  // namespace myclean::tray_model
