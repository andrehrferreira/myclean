#include "svc/backup_component.hpp"

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/backup/backup.hpp"
#include "core/ipc/alerts.hpp"
#include "core/ipc/backup.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/space/scanner.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"
#include "svc/alerts_component.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 11 / 2026} + 12h;

// Restore points only, or a File History copy `history_age` old.
class FakeBackupSystem final : public backup::IBackupSystem {
 public:
  explicit FakeBackupSystem(std::optional<std::chrono::hours> history_age)
      : history_age_(history_age) {}

  [[nodiscard]] Result<backup::FileHistoryFacts> file_history() const override {
    if (!history_age_) {
      return backup::FileHistoryFacts{};
    }
    return backup::FileHistoryFacts{
        .configured = true, .target = "K:", .newest = noon - *history_age_};
  }
  [[nodiscard]] Result<bool> image_versions() const override { return false; }
  [[nodiscard]] Result<std::vector<backup::RestorePoint>> restore_points() const override {
    return std::vector<backup::RestorePoint>{{.created = noon - 24h, .description = "daily"}};
  }
  [[nodiscard]] Result<std::vector<backup::ShadowCopy>> shadow_copies() const override {
    return std::vector<backup::ShadowCopy>{};
  }
  [[nodiscard]] Result<std::vector<backup::OneDriveAccount>> onedrive() const override {
    return std::vector<backup::OneDriveAccount>{};
  }
  [[nodiscard]] Result<std::vector<std::string>> installed_programs() const override {
    return std::vector<std::string>{};
  }
  [[nodiscard]] Result<std::vector<backup::Volume>> volumes() const override {
    return std::vector<backup::Volume>{};
  }

 private:
  std::optional<std::chrono::hours> history_age_;
};

class EmptyReader final : public space::IDirectoryReader {
 public:
  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& /*path*/) const override {
    return fail(Errc::not_found, "no such folder");
  }
};

class BackupComponentTest : public ::testing::Test {
 protected:
  void start(std::optional<std::chrono::hours> history_age) {
    ASSERT_TRUE(alerts_harness_.start(alerts_).has_value());
    backups_ = std::make_unique<BackupComponent>(
        BackupSources{.system =
                          [history_age](std::string_view owner) {
                            EXPECT_EQ(owner, "S-1-5-21-1");
                            return std::make_unique<FakeBackupSystem>(history_age);
                          },
                      .reader = std::make_unique<EmptyReader>(),
                      .period = {}},
        &alerts_);
    ASSERT_TRUE(harness_.start(*backups_, testing::shipped_data, "S-1-5-21-1").has_value());
  }

  [[nodiscard]] static Json ok(const testing::ComponentHarness& harness, std::string_view method,
                               const Json& params = Json::object()) {
    Result<Json> result = harness.call(method, params);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(Json());
  }

  [[nodiscard]] const testing::ComponentHarness& alerts_harness() const { return alerts_harness_; }
  [[nodiscard]] const testing::ComponentHarness& harness() const { return harness_; }
  [[nodiscard]] BackupComponent& backups() { return *backups_; }

 private:
  fakes::FakeClock clock_{noon};
  rules::DiskDataFiles files_;
  testing::ComponentHarness alerts_harness_{clock_};
  AlertsComponent alerts_{files_};
  testing::ComponentHarness harness_{clock_};
  std::unique_ptr<BackupComponent> backups_;
};

// Backup spec, "Restore points alone are not a backup" through the alert engine and
// backups.check.
TEST_F(BackupComponentTest, NoBackupRaisesTheBackupAgeAlert) {
  start(std::nullopt);
  Json answer = ok(harness(), ipc::backups_check_method);
  EXPECT_EQ(answer.at("state"), "ready");
  EXPECT_TRUE(answer.at("checked_at").is_null());

  backups().check_now(std::stop_token{});
  answer = ok(harness(), ipc::backups_check_method);
  EXPECT_EQ(answer.at("summary"), "no backup exists");
  EXPECT_EQ(answer.at("checked_at"), store::to_unix_seconds(noon));
  EXPECT_EQ(answer.at("sources")[2].at("status"), "found");  // the restore points, listed
  EXPECT_TRUE(answer.at("repositories").is_null());          // no git given
  const Json alerts = ok(alerts_harness(), ipc::alerts_list_method).at("alerts");
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_EQ(alerts[0].at("rule"), "backup.age");
  EXPECT_EQ(alerts[0].at("detail"), "no backup exists");
}

// Spec "Stale backup raises one alert" with the shipped limit of 7 days.
TEST_F(BackupComponentTest, AStaleBackupRaisesOneAlertNamingItsSource) {
  start(std::chrono::hours{9 * 24});
  backups().check_now(std::stop_token{});
  backups().check_now(std::stop_token{});
  const Json alerts = ok(alerts_harness(), ipc::alerts_list_method).at("alerts");
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_EQ(alerts[0].at("detail"), "newest backup is 9 days old (File History)");
}

// Spec "Fresh backup raises nothing".
TEST_F(BackupComponentTest, AFreshBackupRaisesNothing) {
  start(std::chrono::hours{2 * 24});
  backups().check_now(std::stop_token{});
  EXPECT_TRUE(ok(alerts_harness(), ipc::alerts_list_method).at("alerts").empty());
  EXPECT_EQ(ok(harness(), ipc::backups_check_method).at("last_backup").at("age_days"), 2);
}

}  // namespace
}  // namespace myclean::svc
