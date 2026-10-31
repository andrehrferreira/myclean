#include "svc/alerts_component.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/database.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;
const std::int64_t noon_seconds = store::to_unix_seconds(noon);
constexpr std::string_view drive = "Samsung SSD 980 PRO 2TB#0123456789abcdef";
constexpr std::string_view drive_token = "Samsung_SSD_980_PRO_2TB_0123456789abcdef";

// The component over the shipped data/alerts.toml.
class AlertsComponentTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(harness_.start(alerts_).has_value()); }

  void report(std::string metric, std::string entity, double value) {
    const std::vector<alerts::Reading> readings{alerts::Reading{
        .metric = std::move(metric), .entity = std::move(entity), .value = value, .detail = "d"}};
    alerts_.report(readings);
  }

  [[nodiscard]] Json ok(std::string_view method, const Json& params = Json::object()) const {
    Result<Json> result = harness_.call(method, params);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(Json());
  }

  [[nodiscard]] const testing::ComponentHarness& harness() const { return harness_; }
  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] AlertsComponent& component() { return alerts_; }

 private:
  fakes::FakeClock clock_{noon};
  rules::DiskDataFiles files_;
  testing::ComponentHarness harness_{clock_};
  AlertsComponent alerts_{files_};
};

TEST_F(AlertsComponentTest, ListsAlertsWithTokenEntitiesAndTheirToastTime) {
  report("volume.free_percent", "C:", 9.0);
  report("volume.free_percent", "D:", 50.0);

  const Json list = ok(ipc::alerts_list_method);
  ASSERT_EQ(list.at("alerts").size(), 1U);
  const Json& alert = list.at("alerts")[0];
  EXPECT_EQ(alert.at("rule"), "volume.free-space-low");
  EXPECT_EQ(alert.at("entity"), "C");
  EXPECT_EQ(alert.at("title"), "Drive is almost full");
  EXPECT_EQ(alert.at("severity"), "warning");
  EXPECT_EQ(alert.at("first_at"), noon_seconds);
  EXPECT_EQ(alert.at("last_at"), noon_seconds);
  EXPECT_EQ(alert.at("count"), 1);
  EXPECT_EQ(alert.at("active"), true);
  EXPECT_TRUE(alert.at("snoozed_until").is_null());
  EXPECT_EQ(alert.at("toast_at"), noon_seconds);
  EXPECT_EQ(alert.at("value"), 9.0);
  EXPECT_EQ(alert.at("limit"), 10.0);
  EXPECT_TRUE(alert.at("previous").is_null());
  EXPECT_EQ(alert.at("unit"), "%");
  EXPECT_EQ(alert.at("detail"), "d");

  EXPECT_EQ(ok(ipc::alerts_badge_method), (Json{{"count", 1}, {"worst", "warning"}}));
  EXPECT_TRUE(ok(ipc::alerts_list_method, Json{{"since", noon_seconds + 1}}).at("alerts").empty());
}

TEST_F(AlertsComponentTest, RepeatedFiringsInsideTheWindowDoNotMoveTheToastTime) {
  report("volume.free_percent", "C:", 9.0);
  clock().advance(1min);
  report("volume.free_percent", "C:", 11.0);  // back within the limit: the alert ends
  clock().advance(1min);
  report("volume.free_percent", "C:", 8.0);  // fires again inside the 5-minute window

  const Json alert = ok(ipc::alerts_list_method).at("alerts").at(0);
  EXPECT_EQ(alert.at("count"), 2);
  EXPECT_EQ(alert.at("toast_at"), noon_seconds);
}

TEST_F(AlertsComponentTest, SnoozeAndDismissResolveTheTokenEntity) {
  report("disk.media_errors", std::string(drive), 0.0);
  report("disk.media_errors", std::string(drive), 1.0);
  report("volume.free_percent", "C:", 9.0);
  EXPECT_EQ(ok(ipc::alerts_badge_method), (Json{{"count", 2}, {"worst", "critical"}}));

  const Json snoozed =
      ok(ipc::alerts_snooze_method,
         Json{{"rule", "disk.media-errors"}, {"entity", drive_token}, {"minutes", 60}});
  EXPECT_EQ(snoozed, (Json{{"snoozed_until", noon_seconds + 3600}}));
  EXPECT_EQ(ok(ipc::alerts_badge_method), (Json{{"count", 1}, {"worst", "warning"}}));
  const Json list = ok(ipc::alerts_list_method);
  for (const Json& alert : list.at("alerts")) {
    EXPECT_EQ(alert.at("snoozed_until"),
              alert.at("rule") == "disk.media-errors" ? Json(noon_seconds + 3600) : Json(nullptr));
  }

  EXPECT_EQ(
      ok(ipc::alerts_dismiss_method, Json{{"rule", "volume.free-space-low"}, {"entity", "C"}}),
      Json::object());
  EXPECT_EQ(ok(ipc::alerts_badge_method), (Json{{"count", 0}, {"worst", "none"}}));

  // A dismissed alert can no longer be dismissed; an unknown one is not found.
  const Result<Json> again = harness().call(
      ipc::alerts_dismiss_method, Json{{"rule", "volume.free-space-low"}, {"entity", "C"}});
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, Errc::not_found);
  const Result<Json> unknown = harness().call(
      ipc::alerts_snooze_method, Json{{"rule", "volume.fill-up"}, {"entity", "Z"}, {"minutes", 5}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, Errc::not_found);
}

TEST_F(AlertsComponentTest, RefusesBadSnoozesAndPathEntities) {
  report("volume.free_percent", "C:", 9.0);
  for (const std::int64_t minutes : {std::int64_t{0}, std::int64_t{ipc::max_snooze_minutes + 1}}) {
    const Result<Json> refused = harness().call(
        ipc::alerts_snooze_method,
        Json{{"rule", "volume.free-space-low"}, {"entity", "C"}, {"minutes", minutes}});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, Errc::invalid_argument);
  }
  EXPECT_EQ(harness().code(ipc::alerts_dismiss_method,
                           Json{{"rule", "volume.free-space-low"}, {"entity", "C:\\"}}),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(harness().code(ipc::alerts_list_method, Json{{"since", "yesterday"}}),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(harness().code(ipc::alerts_list_method, Json{{"since", -1}}),
            ipc::rpc_error::server_error);
}

TEST_F(AlertsComponentTest, IsHealthyWithTheShippedRules) {
  EXPECT_TRUE(component().health().healthy);
  EXPECT_TRUE(component().health().problems.empty());
}

TEST(AlertsComponent, InvalidRulesLeaveTheEngineWithoutRulesAndReportIt) {
  const fakes::FakeClock clock{noon};
  const rules::DiskDataFiles files;
  testing::ComponentHarness harness{clock};
  std::filesystem::create_directories(harness.data_dir());
  std::ofstream(harness.data_dir() / "alerts.toml") << "[[rule]]\nid = \"broken\"\n";
  AlertsComponent alerts{files};
  ASSERT_TRUE(harness.start(alerts, harness.data_dir()).has_value());

  const std::vector<alerts::Reading> readings{
      alerts::Reading{.metric = "volume.free_percent", .entity = "C:", .value = 1.0}};
  alerts.report(readings);
  const Result<Json> list = harness.call(ipc::alerts_list_method);
  ASSERT_TRUE(list.has_value());
  EXPECT_TRUE(list->at("alerts").empty());
  EXPECT_FALSE(alerts.health().healthy);
  ASSERT_EQ(alerts.health().problems.size(), 1U);
  EXPECT_TRUE(alerts.health().problems[0].starts_with("alert rules not loaded"))
      << alerts.health().problems[0];
}

TEST(AlertsComponent, ANotFillingVolumeNeverRaisesAFillUpAlert) {
  const fakes::FakeClock clock{noon};
  const rules::DiskDataFiles files;
  testing::ComponentHarness harness{clock};
  AlertsComponent alerts{files};
  ASSERT_TRUE(harness.start(alerts).has_value());
  const std::vector<alerts::Reading> readings{
      alerts::Reading{.metric = "volume.days_until_full",
                      .entity = "C:",
                      .value = std::numeric_limits<double>::infinity()},
      alerts::Reading{.metric = "volume.days_until_full", .entity = "D:", .value = 9.0}};
  alerts.report(readings);
  const Result<Json> list = harness.call(ipc::alerts_list_method);
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->at("alerts").size(), 1U);
  EXPECT_EQ(list->at("alerts")[0].at("entity"), "D");
  EXPECT_EQ(list->at("alerts")[0].at("rule"), "volume.fill-up");
}

}  // namespace
}  // namespace myclean::svc
