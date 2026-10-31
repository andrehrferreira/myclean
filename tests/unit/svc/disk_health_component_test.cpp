#include "svc/disk_health_component.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/health/analysis.hpp"
#include "core/ipc/disk_health.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

// C: on disk 2, as the telemetry component would report it.
std::shared_ptr<const telemetry::VolumeView> volumes() {
  return std::make_shared<const telemetry::VolumeView>(
      telemetry::VolumeView{.time = noon,
                            .volumes = {telemetry::VolumeStatus{.id = R"(\\?\Volume{c}\)",
                                                                .mount_points = {R"(C:\)"},
                                                                .size = 1000_GiB,
                                                                .free = 100_GiB,
                                                                .disks = {2}}}});
}

class DiskHealthComponentTest : public ::testing::Test {
 protected:
  [[nodiscard]] Result<void> start(const std::filesystem::path& data_dir = testing::shipped_data) {
    return harness_.start(component_, data_dir);
  }

  // disks.health once the first read has completed.
  [[nodiscard]] Json first_read() {
    Json drives;
    EXPECT_TRUE(testing::eventually([&] {
      const Result<Json> reply = harness_.call(ipc::disks_health_method);
      drives = reply.value_or(Json());
      return reply && !reply->at("time").is_null();
    }));
    return drives;
  }

  [[nodiscard]] testing::ComponentHarness& harness() { return harness_; }
  [[nodiscard]] testing::FakeDiskHealthSource& source() { return *source_; }
  [[nodiscard]] testing::RecordingAlertSink& sink() { return sink_; }
  [[nodiscard]] DiskHealthComponent& component() { return component_; }

 private:
  fakes::FakeClock clock_{noon};
  rules::DiskDataFiles files_;
  testing::RecordingAlertSink sink_;
  testing::ComponentHarness harness_{clock_};
  std::unique_ptr<testing::FakeDiskHealthSource> owned_ = [] {
    auto source = std::make_unique<testing::FakeDiskHealthSource>();
    source->probes = {testing::samsung(), testing::usb_blocked()};
    return source;
  }();
  testing::FakeDiskHealthSource* source_ = owned_.get();
  DiskHealthComponent component_{std::move(owned_), files_, sink_, &volumes};
};

TEST_F(DiskHealthComponentTest, ReportsEveryDriveWithItsHealthOrWhyItCannotBeRead) {
  ASSERT_TRUE(start().has_value());
  const Json reply = first_read();
  EXPECT_EQ(reply.at("time"), store::to_unix_seconds(noon));
  const Json& drives = reply.at("drives");
  ASSERT_EQ(drives.size(), 2U);

  const Json& samsung = drives[0];
  EXPECT_EQ(samsung.at("disk"), "2");
  EXPECT_EQ(samsung.at("model"), "Samsung SSD 980 PRO 2TB");
  EXPECT_TRUE(ipc::is_token(samsung.at("key").get<std::string>()));
  EXPECT_TRUE(samsung.at("key").get<std::string>().starts_with("Samsung_SSD_980_PRO_2TB_"));
  EXPECT_EQ(samsung.at("bus"), "nvme");
  EXPECT_EQ(samsung.at("source"), "nvme_health_log");
  EXPECT_EQ(samsung.at("volumes"), Json::array({"C:"}));
  EXPECT_EQ(samsung.at("status"), "good");
  EXPECT_EQ(samsung.at("reasons"), Json::array());
  EXPECT_EQ(samsung.at("readable"), true);
  EXPECT_TRUE(samsung.at("unreadable_reason").is_null());
  EXPECT_EQ(samsung.at("temperature_c"), 40);
  EXPECT_EQ(samsung.at("temperature_limit_c"), 70);
  EXPECT_EQ(samsung.at("available_spare"), 100);
  EXPECT_EQ(samsung.at("media_errors"), 0);
  EXPECT_TRUE(samsung.at("reallocated_sectors").is_null());  // an NVMe drive has none
  EXPECT_EQ(samsung.at("data_written_bytes"), 300'000'000'000'000);
  const Json& endurance = samsung.at("endurance");
  EXPECT_EQ(endurance.at("state"), "insufficient_history");  // the first reading: no rate yet
  EXPECT_EQ(endurance.at("tbw_rated"), 1200.0);
  EXPECT_DOUBLE_EQ(endurance.at("tbw_used_percent").get<double>(), 25.0);
  EXPECT_TRUE(endurance.at("years_left").is_null());
  EXPECT_FALSE(endurance.at("note").get<std::string>().empty());

  const Json& usb = drives[1];
  EXPECT_EQ(usb.at("disk"), "7");
  EXPECT_EQ(usb.at("bus"), "usb");
  EXPECT_EQ(usb.at("volumes"), Json::array());
  EXPECT_EQ(usb.at("status"), "unknown");
  EXPECT_EQ(usb.at("readable"), false);
  EXPECT_EQ(usb.at("unreadable_reason"), "not readable over USB: the bridge rejects pass-through");
  EXPECT_TRUE(usb.at("temperature_c").is_null());
  EXPECT_EQ(usb.at("endurance").at("state"), "not_reported");

  EXPECT_TRUE(component().health().healthy);
}

TEST_F(DiskHealthComponentTest, ReportsTheHealthFactsToTheAlertSink) {
  ASSERT_TRUE(start().has_value());
  static_cast<void>(first_read());
  const std::vector<alerts::Reading> facts = sink().facts();
  const auto media = std::ranges::find_if(facts, [](const alerts::Reading& fact) {
    return fact.metric == health::metric_media_errors;
  });
  ASSERT_NE(media, facts.end());
  EXPECT_TRUE(media->entity.starts_with("Samsung SSD 980 PRO 2TB#"));
  EXPECT_EQ(media->value, 0.0);
}

TEST_F(DiskHealthComponentTest, AFailedReadIsAProblemAndInvalidSpecsAreReported) {
  std::filesystem::create_directories(harness().data_dir());
  std::ofstream(harness().data_dir() / "drive-specs.json") << "{ not json";
  source().failure = Error{.code = Errc::io_error, .message = "no disks"};
  ASSERT_TRUE(start(harness().data_dir()).has_value());

  ASSERT_TRUE(testing::eventually([&] { return component().health().problems.size() == 2; }));
  const ipc::ComponentHealth health = component().health();
  EXPECT_FALSE(health.healthy);
  EXPECT_TRUE(health.problems[0].starts_with("drive specs not loaded")) << health.problems[0];
  EXPECT_EQ(health.problems[1], "disk_health: no disks");
  const Result<Json> reply = harness().call(ipc::disks_health_method);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->at("time").is_null());
  EXPECT_EQ(reply->at("drives"), Json::array());
}

}  // namespace
}  // namespace myclean::svc
