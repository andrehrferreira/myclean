#include "core/health/analysis.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/alerts/config.hpp"
#include "core/alerts/engine.hpp"
#include "core/alerts/schema.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "health_test_support.hpp"

namespace myclean::health {
namespace {

HealthReading nvme(std::int64_t media_errors) {
  return HealthReading{.drive = "Samsung SSD 980 PRO 2TB#00",
                       .model = "Samsung SSD 980 PRO 2TB",
                       .disk_number = 3,
                       .bus = Bus::nvme,
                       .source = Source::nvme_health_log,
                       .temperature_c = 40,
                       .critical_warning = 0,
                       .percentage_used = 5,
                       .available_spare = 100,
                       .spare_threshold = 10,
                       .media_errors = media_errors,
                       .bytes_written = 0};
}

HealthReading sata(std::int64_t reallocated, std::int64_t pending) {
  return HealthReading{.drive = "WDC WD101EFBX-68B0AN0#01",
                       .model = "WDC WD101EFBX-68B0AN0",
                       .disk_number = 0,
                       .bus = Bus::ata,
                       .source = Source::ata_smart,
                       .temperature_c = 40,
                       .reallocated_sectors = reallocated,
                       .pending_sectors = pending,
                       .uncorrectable_sectors = 0,
                       .predicts_failure = false};
}

const DriveSpec samsung{
    .model = "Samsung SSD 980 PRO 2TB", .rated_tbw = 1200, .max_temperature_c = 70, .source = "s"};

// The counter rules of data/alerts.toml, plus the temperature and status rules for the facts
// of alert_readings() that the shipped file does not have yet.
constexpr std::string_view rules_toml = R"(
rate_limit_minutes = 5

[[rule]]
id = "disk.media-errors"
title = "Drive reported new media errors"
metric = "disk.media_errors"
severity = "critical"
change = "increase"

[[rule]]
id = "disk.reallocated-sectors"
title = "Drive replaced damaged sectors"
metric = "disk.reallocated_sectors"
severity = "critical"
change = "increase"

[[rule]]
id = "disk.pending-sectors"
title = "Drive has sectors waiting to be replaced"
metric = "disk.pending_sectors"
severity = "critical"
change = "increase"

[[rule]]
id = "disk.temperature"
title = "Drive is hotter than its rated limit"
metric = "disk.temperature_over_limit"
severity = "warning"
above = 0

[[rule]]
id = "disk.health-degraded"
title = "Drive health is degraded"
metric = "disk.health_status"
severity = "warning"
above = 0.5

[[rule]]
id = "disk.health-failing"
title = "Drive reports it is failing"
metric = "disk.health_status"
severity = "critical"
above = 1.5
)";

// The alert engine as the service runs it, fed with the facts of each health reading.
class AlertRig {
 public:
  AlertRig() {
    const std::array sets{alerts::alert_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
    engine_ = alerts::AlertEngine::open(*store_, clock_,
                                        alerts::parse_alert_file(rules_toml, "alerts.toml").value())
                  .value();
  }

  // Reports the facts of `reading` one read interval after the previous report; returns the
  // toasts the engine shows.
  std::vector<alerts::Alert> report(const HealthReading& reading, const DriveSpec* spec) {
    clock_.advance(std::chrono::minutes{10});
    const std::vector<alerts::Reading> facts = alert_readings(reading, assess(reading, spec));
    return engine_->report(facts).value();
  }

 private:
  testing::TempFolder folder_;
  fakes::FakeClock clock_{WallTime{std::chrono::sys_days{std::chrono::year{2026} / 10 / 1}}};
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<alerts::AlertEngine> engine_;
};

const alerts::Alert* toast(const std::vector<alerts::Alert>& toasts, std::string_view rule) {
  const auto found = std::ranges::find_if(
      toasts, [&](const alerts::Alert& alert) { return alert.key.rule == rule; });
  return found == toasts.end() ? nullptr : &*found;
}

const alerts::Reading* fact(const std::vector<alerts::Reading>& facts, std::string_view metric) {
  const auto found = std::ranges::find_if(
      facts, [&](const alerts::Reading& reading) { return reading.metric == metric; });
  return found == facts.end() ? nullptr : &*found;
}

TEST(Assess, HealthyDriveIsGood) {
  const Assessment result = assess(nvme(0), &samsung);
  EXPECT_EQ(result.status, Status::good);
  EXPECT_TRUE(result.reasons.empty());
  EXPECT_EQ(result.temperature_limit_c, 70);
}

TEST(Assess, AnyMediaErrorOrSectorCounterIsAWarning) {
  EXPECT_EQ(assess(nvme(498), &samsung).status, Status::warning);
  EXPECT_EQ(assess(nvme(498), &samsung).reasons, std::vector<std::string>{"498 media errors"});
  EXPECT_EQ(assess(sata(1, 0), nullptr).status, Status::warning);
  EXPECT_EQ(assess(sata(0, 2), nullptr).status, Status::warning);
}

TEST(Assess, DriveOwnFailureSignalsAreCritical) {
  HealthReading spare = nvme(0);
  spare.available_spare = 5;
  EXPECT_EQ(assess(spare, &samsung).status, Status::critical);

  HealthReading warning_bits = nvme(0);
  warning_bits.critical_warning = 0x04;
  const Assessment bits = assess(warning_bits, &samsung);
  EXPECT_EQ(bits.status, Status::critical);
  EXPECT_EQ(bits.reasons.front(),
            "NVMe critical warning 0x04: reliability degraded by media errors");

  HealthReading worn = nvme(0);
  worn.percentage_used = 100;
  EXPECT_EQ(assess(worn, &samsung).status, Status::critical);

  HealthReading predicts = sata(0, 0);
  predicts.predicts_failure = true;
  EXPECT_EQ(assess(predicts, nullptr).status, Status::critical);
}

TEST(Assess, UnreadableDriveIsUnknownWithItsReason) {
  const HealthReading reading{.model = "ROG ESD-S1C",
                              .bus = Bus::usb,
                              .source = Source::none,
                              .problem = "not readable over USB: blocked"};
  const Assessment result = assess(reading, nullptr);
  EXPECT_EQ(result.status, Status::unknown);
  EXPECT_EQ(result.reasons, std::vector<std::string>{"not readable over USB: blocked"});
}

TEST(AlertReadings, CountersAreReportedUnderTheStableDriveIdentity) {
  const HealthReading reading = nvme(1);
  const std::vector<alerts::Reading> facts = alert_readings(reading, assess(reading, &samsung));
  const alerts::Reading* media = fact(facts, metric_media_errors);
  ASSERT_NE(media, nullptr);
  EXPECT_EQ(media->entity, "Samsung SSD 980 PRO 2TB#00");
  EXPECT_EQ(media->value, 1.0);
  EXPECT_EQ(media->detail, "Samsung SSD 980 PRO 2TB (disk 3)");
  EXPECT_EQ(fact(facts, metric_reallocated_sectors), nullptr);  // not an ATA counter on NVMe

  const HealthReading hdd = sata(3, 2);
  const std::vector<alerts::Reading> sata_facts = alert_readings(hdd, assess(hdd, nullptr));
  EXPECT_EQ(fact(sata_facts, metric_reallocated_sectors)->value, 3.0);
  EXPECT_EQ(fact(sata_facts, metric_pending_sectors)->value, 2.0);
  EXPECT_EQ(fact(sata_facts, metric_media_errors), nullptr);
}

TEST(AlertReadings, StatusIsReportedWithItsReasonsUnlessUnknown) {
  const HealthReading warned = nvme(498);
  const std::vector<alerts::Reading> facts = alert_readings(warned, assess(warned, &samsung));
  const alerts::Reading* status = fact(facts, metric_health_status);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->value, 1.0);
  EXPECT_EQ(status->detail, "Samsung SSD 980 PRO 2TB (disk 3): warning (498 media errors)");

  const HealthReading unreadable{.drive = "ROG ESD-S1C#02", .model = "ROG ESD-S1C"};
  EXPECT_TRUE(alert_readings(unreadable, assess(unreadable, nullptr)).empty());
}

TEST(AlertRules, FirstMediaErrorNamesTheDriveTheCounterAndBothValues) {
  AlertRig rig;
  EXPECT_TRUE(rig.report(nvme(0), &samsung).empty());
  const std::vector<alerts::Alert> toasts = rig.report(nvme(1), &samsung);
  const alerts::Alert* media = toast(toasts, "disk.media-errors");
  ASSERT_NE(media, nullptr);
  EXPECT_EQ(media->key.entity, "Samsung SSD 980 PRO 2TB#00");
  EXPECT_EQ(media->detail, "Samsung SSD 980 PRO 2TB (disk 3)");
  EXPECT_EQ(media->previous, 0.0);
  EXPECT_EQ(media->value, 1.0);
}

TEST(AlertRules, NewPendingSectorsAlertAlthoughNoThresholdIsExceeded) {
  AlertRig rig;
  EXPECT_TRUE(rig.report(sata(0, 0), nullptr).empty());
  const std::vector<alerts::Alert> toasts = rig.report(sata(0, 2), nullptr);
  const alerts::Alert* pending = toast(toasts, "disk.pending-sectors");
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->previous, 0.0);
  EXPECT_EQ(pending->value, 2.0);
}

TEST(AlertRules, UnchangedCounterRaisesNothingNew) {
  AlertRig rig;
  const std::vector<alerts::Alert> first = rig.report(sata(3, 0), nullptr);
  EXPECT_EQ(toast(first, "disk.reallocated-sectors"), nullptr);  // no earlier reading
  EXPECT_NE(toast(first, "disk.health-degraded"), nullptr);
  EXPECT_TRUE(rig.report(sata(3, 0), nullptr).empty());
}

TEST(AlertRules, TemperatureAboveTheModelLimitNamesTemperatureAndLimit) {
  AlertRig rig;
  HealthReading hot = nvme(0);
  hot.temperature_c = 74;
  const std::vector<alerts::Alert> toasts = rig.report(hot, &samsung);
  const alerts::Alert* alert = toast(toasts, "disk.temperature");
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->detail, "Samsung SSD 980 PRO 2TB (disk 3): 74 °C, rated limit 70 °C");
}

TEST(AlertRules, UnknownModelShowsTemperatureStatesNoLimitAndRaisesNothing) {
  AlertRig rig;
  HealthReading hot = nvme(0);
  hot.model = "Unknown NVMe";
  hot.temperature_c = 95;
  EXPECT_TRUE(rig.report(hot, nullptr).empty());
  const Assessment result = assess(hot, nullptr);
  EXPECT_FALSE(result.temperature_limit_c.has_value());
  EXPECT_EQ(result.temperature_note, "no rated limit known for Unknown NVMe");
}

TEST(AlertRules, FailingDriveRaisesACriticalAlert) {
  AlertRig rig;
  EXPECT_TRUE(rig.report(nvme(0), &samsung).empty());
  HealthReading failing = nvme(0);
  failing.available_spare = 1;
  const std::vector<alerts::Alert> toasts = rig.report(failing, &samsung);
  const alerts::Alert* alert = toast(toasts, "disk.health-failing");
  ASSERT_NE(alert, nullptr);
  EXPECT_EQ(alert->severity, alerts::Severity::critical);
}

TEST(AlertRules, ShippedRulesWatchEveryCounterFact) {
  const rules::DiskDataFiles files;
  const Result<alerts::AlertConfig> config = alerts::load_alert_config(files, MYCLEAN_HEALTH_DATA);
  ASSERT_TRUE(config.has_value()) << config.error().message;
  for (const std::string_view metric :
       {metric_media_errors, metric_reallocated_sectors, metric_pending_sectors}) {
    EXPECT_TRUE(std::ranges::any_of(config->rules, [&](const alerts::AlertRule& rule) {
      return rule.metric == metric && rule.change == alerts::Change::increase;
    })) << metric;
  }
}

TEST(Endurance, KnownRatingProjectsUsedShareAndYearsLeft) {
  const Endurance result = project_endurance(std::int64_t{300} * 1'000'000'000'000, 1200, 50);
  EXPECT_DOUBLE_EQ(*result.written_tb, 300.0);
  EXPECT_DOUBLE_EQ(*result.used_percent, 25.0);
  EXPECT_DOUBLE_EQ(*result.years_left, 18.0);
  EXPECT_TRUE(result.unavailable.empty());
}

TEST(Endurance, UnknownRatingShowsDataWrittenAndSaysWhy) {
  const Endurance result =
      project_endurance(std::int64_t{300} * 1'000'000'000'000, std::nullopt, 50);
  EXPECT_DOUBLE_EQ(*result.written_tb, 300.0);
  EXPECT_FALSE(result.years_left.has_value());
  EXPECT_EQ(result.unavailable, "projection unavailable: the rated TBW of this model is unknown");
}

TEST(Endurance, MissingRateOrExhaustedRating) {
  const Endurance no_rate = project_endurance(std::int64_t{1'000'000'000'000}, 1200, std::nullopt);
  EXPECT_FALSE(no_rate.years_left.has_value());
  EXPECT_EQ(no_rate.unavailable, "projection unavailable: the write rate needs 7 days of history");

  const Endurance used_up = project_endurance(std::int64_t{1300} * 1'000'000'000'000, 1200, 10);
  EXPECT_DOUBLE_EQ(*used_up.years_left, 0.0);

  const Endurance hdd = project_endurance(std::nullopt, std::nullopt, std::nullopt);
  EXPECT_EQ(hdd.unavailable, "the drive does not report the data written");
}

TEST(WriteRate, NeedsSevenDaysOfHistory) {
  const WallTime start{std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};
  const WrittenAt first{.time = start, .bytes = 0};
  EXPECT_FALSE(write_rate_tb_per_year(
                   first, {.time = start + std::chrono::days{6}, .bytes = 1'000'000'000'000})
                   .has_value());
  // 1 TB in 36.5 days is 10 TB per year (365.2425 days).
  const std::optional<double> rate = write_rate_tb_per_year(
      first, {.time = start + std::chrono::hours{876}, .bytes = 1'000'000'000'000});
  ASSERT_TRUE(rate.has_value());
  EXPECT_NEAR(*rate, 10.0, 0.01);
}

}  // namespace
}  // namespace myclean::health
