#include "core/alerts/engine.hpp"

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/alerts/config.hpp"
#include "core/alerts/schema.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::alerts {
namespace {

using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::seconds;

// 2026-10-31 12:00:00 UTC.
const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + hours{12};

AlertConfig config() {
  return AlertConfig{.rate_limit = minutes{5},
                     .rules = {
                         {.id = "volume.free-space-low",
                          .title = "Drive is almost full",
                          .metric = "volume.free_percent",
                          .severity = Severity::warning,
                          .below = 10.0,
                          .unit = "%"},
                         {.id = "volume.fill-up",
                          .title = "Drive will be full soon",
                          .metric = "volume.days_until_full",
                          .severity = Severity::warning,
                          .below = 14.0,
                          .unit = "days"},
                         {.id = "disk.media-errors",
                          .title = "Drive reported new media errors",
                          .metric = "disk.media_errors",
                          .severity = Severity::critical,
                          .change = Change::increase},
                         {.id = "disk.temperature",
                          .title = "Drive is hot",
                          .metric = "disk.temperature",
                          .severity = Severity::critical,
                          .above = 70.0,
                          .unit = "C"},
                     }};
}

// A fresh folder under %TEMP% for the current test, removed afterwards.
class TempFolder {
 public:
  TempFolder()
      : path_(std::filesystem::temp_directory_path() /
              ("myclean-alerts-" +
               std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  TempFolder(TempFolder&&) = delete;
  TempFolder& operator=(TempFolder&&) = delete;
  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class AlertEngineTest : public ::testing::Test {
 protected:
  AlertEngineTest() { reopen(); }

  // A service restart: the store and the engine are opened again from the database file.
  void reopen() {
    engine_.reset();
    store_.reset();
    const std::vector<store::MigrationSet> sets = {alert_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
    Result<std::unique_ptr<AlertEngine>> opened = AlertEngine::open(*store_, clock_, config());
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    engine_ = std::move(*opened);
  }

  // Reports one reading; the toasts it produced.
  std::vector<Alert> report(const std::string& metric, const std::string& entity, double value,
                            const std::string& detail = {}) {
    const std::vector<Reading> readings = {
        {.metric = metric, .entity = entity, .value = value, .detail = detail}};
    Result<std::vector<Alert>> toasts = engine_->report(readings);
    EXPECT_TRUE(toasts.has_value()) << toasts.error().message;
    return toasts.value_or(std::vector<Alert>{});
  }

  std::vector<Alert> stored() {
    Result<std::vector<Alert>> alerts = engine_->list(WallTime{});
    EXPECT_TRUE(alerts.has_value()) << alerts.error().message;
    return alerts.value_or(std::vector<Alert>{});
  }

  fakes::FakeClock& clock() { return clock_; }
  AlertEngine& engine() { return *engine_; }
  store::Store& db() { return *store_; }

 private:
  TempFolder folder_;  // destroyed last, after the engine and the store
  fakes::FakeClock clock_{noon};
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<AlertEngine> engine_;
};

TEST(AlertMigrations, AreAdditive) {
  const std::vector<store::MigrationSet> sets = {store::store_migrations(), alert_migrations()};
  const Result<void> checked = store::check_additive(sets);
  EXPECT_TRUE(checked.has_value()) << checked.error().message;
}

TEST_F(AlertEngineTest, ThresholdRuleRaisesAlertWithValueAndLimit) {
  EXPECT_TRUE(report("volume.free_percent", "C:", 12.0).empty());
  const std::vector<Alert> toasts = report("volume.free_percent", "C:", 9.0);
  ASSERT_EQ(toasts.size(), 1U);
  EXPECT_EQ(toasts[0].key.rule, "volume.free-space-low");
  EXPECT_EQ(toasts[0].key.entity, "C:");
  EXPECT_EQ(toasts[0].title, "Drive is almost full");
  EXPECT_EQ(toasts[0].severity, Severity::warning);
  EXPECT_EQ(toasts[0].value, 9.0);
  EXPECT_EQ(toasts[0].limit, 10.0);
  EXPECT_EQ(toasts[0].unit, "%");
  EXPECT_EQ(toasts[0].count, 1);
  EXPECT_TRUE(toasts[0].active);
  EXPECT_EQ(engine().badge().count, 1);
}

TEST_F(AlertEngineTest, ReadingBackWithinLimitEndsTheAlert) {
  report("volume.free_percent", "C:", 9.0);
  EXPECT_TRUE(report("volume.free_percent", "C:", 8.0).empty());  // still beyond: no new firing
  report("volume.free_percent", "C:", 11.0);
  EXPECT_TRUE(engine().active().empty());
  EXPECT_EQ(engine().badge().count, 0);
  const std::vector<Alert> alerts = stored();
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_FALSE(alerts[0].active);
  EXPECT_EQ(alerts[0].count, 1);
}

TEST_F(AlertEngineTest, ChangeRuleRaisesAlertWithoutThreshold) {
  EXPECT_TRUE(report("disk.media_errors", "2", 0.0).empty());  // the first reading is the baseline
  clock().advance(minutes{10});
  const std::vector<Alert> toasts = report("disk.media_errors", "2", 1.0);
  ASSERT_EQ(toasts.size(), 1U);
  EXPECT_EQ(toasts[0].key.rule, "disk.media-errors");
  EXPECT_EQ(toasts[0].value, 1.0);
  EXPECT_EQ(toasts[0].previous, 0.0);
  EXPECT_FALSE(toasts[0].limit.has_value());
  EXPECT_EQ(engine().badge().worst, Severity::critical);
}

TEST_F(AlertEngineTest, IncreaseRuleIgnoresDecreaseAndRepeats) {
  report("disk.media_errors", "2", 5.0);
  EXPECT_TRUE(report("disk.media_errors", "2", 5.0).empty());
  EXPECT_TRUE(report("disk.media_errors", "2", 3.0).empty());
  EXPECT_TRUE(stored().empty());
}

TEST_F(AlertEngineTest, ChangeBaselineSurvivesARestart) {
  report("disk.media_errors", "2", 0.0);
  reopen();
  clock().advance(hours{1});
  EXPECT_EQ(report("disk.media_errors", "2", 1.0).size(), 1U);
}

TEST_F(AlertEngineTest, VolumeAboutToFillNamesVolumeFreeSpaceAndProjection) {
  const std::vector<Alert> toasts = report("volume.days_until_full", "C:", 9.0, "38.2 GiB free");
  ASSERT_EQ(toasts.size(), 1U);
  EXPECT_EQ(toasts[0].key.entity, "C:");
  EXPECT_EQ(toasts[0].value, 9.0);
  EXPECT_EQ(toasts[0].limit, 14.0);
  EXPECT_EQ(toasts[0].unit, "days");
  EXPECT_EQ(toasts[0].detail, "38.2 GiB free");
}

TEST_F(AlertEngineTest, VolumeNotFillingRaisesNoAlert) {
  EXPECT_TRUE(
      report("volume.days_until_full", "D:", std::numeric_limits<double>::infinity()).empty());
  EXPECT_TRUE(
      report("volume.days_until_full", "D:", std::numeric_limits<double>::quiet_NaN()).empty());
  EXPECT_TRUE(stored().empty());
}

TEST_F(AlertEngineTest, ListingReturnsEveryAlertWithItsFields) {
  report("volume.free_percent", "C:", 9.0);
  clock().advance(minutes{1});
  report("volume.days_until_full", "D:", 3.0);
  clock().advance(minutes{1});
  report("disk.media_errors", "4", 0.0);
  report("disk.media_errors", "4", 2.0);
  const std::vector<Alert> alerts = stored();
  ASSERT_EQ(alerts.size(), 3U);
  EXPECT_EQ(alerts[0].key.rule, "disk.media-errors");  // newest first
  EXPECT_EQ(alerts[0].key.entity, "4");
  EXPECT_EQ(alerts[0].severity, Severity::critical);
  EXPECT_EQ(alerts[0].value, 2.0);
  EXPECT_EQ(alerts[0].previous, 0.0);
  EXPECT_EQ(alerts[0].first, noon + minutes{2});
  EXPECT_EQ(alerts[0].last, noon + minutes{2});
  EXPECT_EQ(alerts[1].key.rule, "volume.fill-up");
  EXPECT_EQ(alerts[2].key.rule, "volume.free-space-low");
  EXPECT_EQ(alerts[2].limit, 10.0);
  EXPECT_EQ(alerts[2].first, noon);
  ASSERT_TRUE(engine().list(noon + minutes{1}).has_value());
  EXPECT_EQ(engine().list(noon + minutes{1})->size(), 2U);
}

TEST_F(AlertEngineTest, AlertRaisedWhileTrayIsNotRunningIsKeptForTheBadge) {
  report("disk.media_errors", "3", 0.0);
  report("disk.media_errors", "3", 1.0);
  reopen();  // whoever asks later (the tray, after it starts) sees the stored, active alert
  const Badge badge = engine().badge();
  EXPECT_EQ(badge.count, 1);
  EXPECT_EQ(badge.worst, Severity::critical);
  ASSERT_EQ(engine().active().size(), 1U);
  EXPECT_EQ(engine().active()[0].key.entity, "3");
}

TEST_F(AlertEngineTest, ActiveThresholdAlertIsNotRaisedAgainAfterARestart) {
  report("volume.free_percent", "C:", 9.0);
  reopen();
  clock().advance(hours{1});
  EXPECT_TRUE(report("volume.free_percent", "C:", 8.0).empty());
  report("volume.free_percent", "C:", 20.0);
  EXPECT_TRUE(engine().active().empty());
}

TEST_F(AlertEngineTest, FlappingThresholdShowsOneToastAndCountsEveryOccurrence) {
  int toasts = 0;
  for (int firing = 0; firing < 12; ++firing) {
    toasts += static_cast<int>(report("volume.free_percent", "C:", 9.0).size());
    clock().advance(seconds{10});
    report("volume.free_percent", "C:", 11.0);
    clock().advance(seconds{10});
  }
  EXPECT_EQ(toasts, 1);
  const std::vector<Alert> alerts = stored();
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_EQ(alerts[0].count, 12);
  EXPECT_EQ(alerts[0].first, noon);
  EXPECT_EQ(alerts[0].last, noon + seconds{11 * 20});
}

TEST_F(AlertEngineTest, DifferentEntitiesAreRateLimitedIndependently) {
  EXPECT_EQ(report("disk.temperature", "3", 75.0).size(), 1U);
  clock().advance(seconds{30});
  EXPECT_EQ(report("disk.temperature", "5", 76.0).size(), 1U);
}

TEST_F(AlertEngineTest, FiringAfterTheWindowIsANewAlertWithAToast) {
  report("volume.free_percent", "C:", 9.0);
  report("volume.free_percent", "C:", 11.0);
  clock().advance(minutes{6});
  EXPECT_EQ(report("volume.free_percent", "C:", 9.0).size(), 1U);
  EXPECT_EQ(stored().size(), 2U);
}

TEST_F(AlertEngineTest, SnoozedAlertFiringAgainIsRecordedWithoutToastOrBadge) {
  report("volume.days_until_full", "C:", 9.0);
  ASSERT_TRUE(engine().snooze({.rule = "volume.fill-up", .entity = "C:"}, hours{24}).has_value());
  EXPECT_EQ(engine().badge().count, 0);
  report("volume.days_until_full", "C:", std::numeric_limits<double>::infinity());
  clock().advance(hours{2});
  EXPECT_TRUE(report("volume.days_until_full", "C:", 8.0).empty());
  EXPECT_EQ(engine().badge().count, 0);
  ASSERT_EQ(engine().active().size(), 1U);
  EXPECT_EQ(engine().active()[0].snoozed_until, noon + hours{24});
  const std::vector<Alert> alerts = stored();
  ASSERT_EQ(alerts.size(), 2U);
  EXPECT_EQ(alerts[0].last, noon + hours{2});
  // The listing carries the snooze of the key, on every record of it, until it expires.
  EXPECT_EQ(alerts[0].snoozed_until, noon + hours{24});
  EXPECT_EQ(alerts[1].snoozed_until, noon + hours{24});
  clock().advance(hours{22});
  EXPECT_FALSE(stored()[0].snoozed_until.has_value());
}

TEST_F(AlertEngineTest, ToastIsShownAgainWhenTheSnoozeHasExpired) {
  report("volume.days_until_full", "C:", 9.0);
  ASSERT_TRUE(engine().snooze({.rule = "volume.fill-up", .entity = "C:"}, hours{1}).has_value());
  report("volume.days_until_full", "C:", 20.0);
  clock().advance(hours{1});
  EXPECT_EQ(report("volume.days_until_full", "C:", 9.0).size(), 1U);
  EXPECT_EQ(engine().badge().count, 1);
}

TEST_F(AlertEngineTest, SnoozeSurvivesARestart) {
  ASSERT_TRUE(engine().snooze({.rule = "volume.fill-up", .entity = "C:"}, hours{24}).has_value());
  reopen();
  clock().advance(hours{1});
  EXPECT_TRUE(report("volume.days_until_full", "C:", 9.0).empty());
}

TEST_F(AlertEngineTest, SnoozeIsPerAlert) {
  report("volume.days_until_full", "C:", 9.0);
  ASSERT_TRUE(engine().snooze({.rule = "volume.fill-up", .entity = "C:"}, hours{24}).has_value());
  report("disk.media_errors", "2", 0.0);
  EXPECT_EQ(report("disk.media_errors", "2", 1.0).size(), 1U);
  EXPECT_EQ(report("volume.days_until_full", "D:", 5.0).size(), 1U);
  EXPECT_EQ(engine().badge().count, 2);
}

TEST_F(AlertEngineTest, SnoozeNeedsAPositiveDuration) {
  const Result<void> snoozed =
      engine().snooze({.rule = "volume.fill-up", .entity = "C:"}, seconds{0});
  ASSERT_FALSE(snoozed.has_value());
  EXPECT_EQ(snoozed.error().code, Errc::invalid_argument);
}

TEST_F(AlertEngineTest, DismissedAlertLeavesTheActiveSetUntilItFiresAgain) {
  report("disk.media_errors", "2", 0.0);
  report("disk.media_errors", "2", 1.0);
  ASSERT_TRUE(engine().dismiss({.rule = "disk.media-errors", .entity = "2"}).has_value());
  EXPECT_EQ(engine().badge().count, 0);
  EXPECT_EQ(engine().dismiss({.rule = "disk.media-errors", .entity = "2"}).error().code,
            Errc::not_found);
  clock().advance(hours{1});
  EXPECT_EQ(report("disk.media_errors", "2", 2.0).size(), 1U);
  EXPECT_EQ(engine().badge().count, 1);
}

TEST_F(AlertEngineTest, BadgeShowsTheWorstSeverity) {
  report("volume.free_percent", "C:", 9.0);
  EXPECT_EQ(engine().badge().worst, Severity::warning);
  report("disk.temperature", "1", 80.0);
  EXPECT_EQ(engine().badge().count, 2);
  EXPECT_EQ(engine().badge().worst, Severity::critical);
}

TEST_F(AlertEngineTest, AlertsAreKeptTwoYears) {
  report("volume.free_percent", "C:", 9.0);
  report("volume.free_percent", "C:", 11.0);
  clock().advance(store::Store::history_retention - hours{1});
  ASSERT_TRUE(db().run_retention().has_value());
  EXPECT_EQ(stored().size(), 1U);
  clock().advance(hours{2});
  ASSERT_TRUE(db().run_retention().has_value());
  EXPECT_TRUE(stored().empty());
}

TEST_F(AlertEngineTest, NewRulesApplyAfterReload) {
  AlertConfig changed = config();
  changed.rules[0].below = 20.0;
  engine().set_config(changed);
  EXPECT_EQ(report("volume.free_percent", "C:", 15.0).size(), 1U);
}

}  // namespace
}  // namespace myclean::alerts
