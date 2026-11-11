#include "core/health/collector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/alerts/engine.hpp"
#include "core/health/analysis.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/history.hpp"
#include "core/health/reading.hpp"
#include "core/health/source.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"
#include "fakes/fake_clock.hpp"
#include "health_test_support.hpp"

namespace myclean::health {
namespace {

using std::chrono::days;
using std::chrono::hours;
using std::chrono::minutes;

const WallTime start{std::chrono::sys_days{std::chrono::year{2026} / 10 / 1} + hours{12}};
constexpr std::int64_t terabyte = 1'000'000'000'000;
constexpr std::int64_t nvme_data_unit = 512'000;

class FakeSource final : public IDiskHealthSource {
 public:
  [[nodiscard]] Result<std::vector<DriveProbe>> probe() const override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    return probes;
  }

  std::vector<DriveProbe> probes;
  std::optional<Error> failure;
};

class RecordingSink final : public IAlertSink {
 public:
  void report(std::span<const alerts::Reading> readings) override {
    facts.insert(facts.end(), readings.begin(), readings.end());
  }

  [[nodiscard]] const alerts::Reading* find(std::string_view metric) const {
    const auto found = std::ranges::find_if(
        facts, [&](const alerts::Reading& fact) { return fact.metric == metric; });
    return found == facts.end() ? nullptr : &*found;
  }

  std::vector<alerts::Reading> facts;
};

// A 512-byte NVMe health log with the given counters (and a valid composite temperature).
std::vector<std::uint8_t> nvme_log(std::int64_t media_errors, std::int64_t bytes_written,
                                   std::uint16_t kelvin = 313) {
  std::vector<std::uint8_t> log(512, 0);
  const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
      log.at(offset + index) = static_cast<std::uint8_t>(value >> (8 * index));
    }
  };
  put(1, kelvin, 2);
  log.at(3) = 100;
  log.at(4) = 10;
  put(48, static_cast<std::uint64_t>(bytes_written / nvme_data_unit), 8);
  put(160, static_cast<std::uint64_t>(media_errors), 8);
  return log;
}

DriveProbe samsung(std::int64_t media_errors, std::int64_t bytes_written = 0) {
  return DriveProbe{.disk_number = 3,
                    .model = "Samsung SSD 980 PRO 2TB",
                    .serial = "S1",
                    .bus = Bus::nvme,
                    .nvme_health_log = nvme_log(media_errors, bytes_written)};
}

DriveProbe usb_blocked() {
  return DriveProbe{.disk_number = 7,
                    .model = "ROG ESD-S1C",
                    .serial = "U1",
                    .bus = Bus::usb,
                    .problem =
                        "not readable over USB: SAT pass-through needs administrator "
                        "rights (Win32 error 5)"};
}

DriveSpecs specs() {
  Result<DriveSpecs> parsed = DriveSpecs::parse(
      R"({"drives": [{"model": "Samsung SSD 980 PRO 2TB", "rated_tbw": 1200,
                      "max_temperature_c": 70, "source": "data sheet"}]})",
      "drive-specs.json");
  EXPECT_TRUE(parsed.has_value());
  return *std::move(parsed);
}

// The collector over a fake source, a real store in a temporary folder and a recording sink.
class HealthCollectorTest : public ::testing::Test {
 protected:
  HealthCollectorTest() { reopen(); }

  // Closes and reopens the store and the collector, as a service restart does.
  void reopen() {
    collector_.reset();
    store_.reset();
    const std::array sets{health_migrations()};
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(folder_.path() / "myclean.db", clock_, sets);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    Result<std::unique_ptr<HealthCollector>> created =
        HealthCollector::create(source_, *store_, specs(), sink_, clock_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    collector_ = std::move(*created);
  }

  Result<std::vector<telemetry::Measurement>> poll() {
    return collector_->collect(clock_.now(), clock_.monotonic());
  }

  std::vector<Snapshot> history(const std::string& drive) {
    Result<std::vector<Snapshot>> rows =
        collector_->history(drive, start - days{1}, start + days{400});
    EXPECT_TRUE(rows.has_value()) << rows.error().message;
    return rows.value_or(std::vector<Snapshot>{});
  }

  [[nodiscard]] static std::string samsung_id() {
    return "Samsung SSD 980 PRO 2TB#" + serial_hash("S1");
  }

  HealthCollector& collector() { return *collector_; }
  fakes::FakeClock& clock() { return clock_; }
  FakeSource& source() { return source_; }
  RecordingSink& sink() { return sink_; }

 private:
  testing::TempFolder folder_;  // first, so that it is removed last
  fakes::FakeClock clock_{start};
  FakeSource source_;
  RecordingSink sink_;
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<HealthCollector> collector_;
};

TEST_F(HealthCollectorTest, ReadsEveryTenMinutesAsAPipelineCollector) {
  EXPECT_EQ(collector().interval(), std::chrono::seconds{600});
  EXPECT_EQ(collector().name(), "disk_health");
  EXPECT_EQ(collector().latest(), nullptr);
}

TEST_F(HealthCollectorTest, EveryDriveHasAReadingOrAnExplicitReasonWithItsSource) {
  source().probes = {usb_blocked(), samsung(0)};
  const Result<std::vector<telemetry::Measurement>> measured = poll();
  ASSERT_TRUE(measured.has_value()) << measured.error().message;

  const std::shared_ptr<const HealthView> view = collector().latest();
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->drives.size(), 2U);
  const DriveHealth& nvme = view->drives[0];  // disk number order
  EXPECT_EQ(nvme.reading.source, Source::nvme_health_log);
  EXPECT_EQ(nvme.assessment.status, Status::good);
  EXPECT_EQ(nvme.reading.temperature_c, 40);
  const DriveHealth& usb = view->drives[1];
  EXPECT_EQ(usb.reading.source, Source::none);
  EXPECT_EQ(usb.assessment.status, Status::unknown);
  EXPECT_TRUE(usb.reading.problem.starts_with("not readable over USB"));
  EXPECT_EQ(usb.assessment.temperature_note, "no rated limit known for ROG ESD-S1C");

  ASSERT_EQ(measured->size(), 1U);
  EXPECT_EQ(measured->front().key.metric, metric_disk_temperature);
  EXPECT_EQ(measured->front().key.entity, nvme.reading.drive);  // stable, not the disk number
  EXPECT_TRUE(nvme.reading.drive.starts_with(nvme.reading.model + "#"));
  EXPECT_EQ(measured->front().value, 40.0);
}

TEST_F(HealthCollectorTest, StoresOneSnapshotPerDriveAndDay) {
  source().probes = {samsung(0)};
  for (int day = 0; day < 3; ++day) {
    for (int read = 0; read < 4; ++read) {
      ASSERT_TRUE(poll().has_value());
      clock().advance(minutes{10});
    }
    clock().advance(days{1} - minutes{40});
  }
  const std::vector<Snapshot> rows = history(samsung_id());
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(rows[0].day, "2026-10-01");
  EXPECT_EQ(rows[1].day, "2026-10-02");
  EXPECT_EQ(rows[2].day, "2026-10-03");
  // The snapshot of a day is its last reading.
  EXPECT_EQ(rows[0].time, start + minutes{30});
  EXPECT_EQ(rows[0].status, Status::good);
  EXPECT_EQ(rows[0].reading.media_errors, 0);
  EXPECT_EQ(rows[0].reading.source, Source::nvme_health_log);
}

TEST_F(HealthCollectorTest, MissedDayGetsNoInventedSnapshot) {
  source().probes = {samsung(0)};
  ASSERT_TRUE(poll().has_value());
  clock().advance(days{2});  // the PC was off on 2026-10-02
  reopen();
  ASSERT_TRUE(poll().has_value());

  const std::vector<Snapshot> rows = history(samsung_id());
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].day, "2026-10-01");
  EXPECT_EQ(rows[1].day, "2026-10-03");
}

TEST_F(HealthCollectorTest, ReportsTheAlertFactsOfEveryReadingToTheSink) {
  DriveProbe hot = samsung(1);
  hot.nvme_health_log = nvme_log(1, 0, 273 + 74);
  source().probes = {hot, usb_blocked()};
  ASSERT_TRUE(poll().has_value());

  const alerts::Reading* media = sink().find(metric_media_errors);
  ASSERT_NE(media, nullptr);
  EXPECT_EQ(media->entity, samsung_id());
  EXPECT_EQ(media->value, 1.0);
  const alerts::Reading* temperature = sink().find(metric_temperature_over_limit);
  ASSERT_NE(temperature, nullptr);
  EXPECT_EQ(temperature->value, 4.0);
  EXPECT_EQ(sink().find(metric_health_status)->value, 1.0);
  // The unreadable USB drive reports nothing an alert could be raised from.
  EXPECT_TRUE(std::ranges::none_of(sink().facts, [](const alerts::Reading& fact) {
    return fact.entity.starts_with("ROG ESD-S1C#");
  }));
}

TEST_F(HealthCollectorTest, EnduranceIsProjectedFromTheLastYearOfSnapshots) {
  source().probes = {samsung(0, 300 * terabyte)};
  ASSERT_TRUE(poll().has_value());
  EXPECT_EQ(collector().latest()->drives[0].endurance.unavailable,
            "projection unavailable: the write rate needs 7 days of history");

  // 50 TB a year: 1 TB every 7.3 days (365.2425 / 50).
  clock().advance(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::duration<double, std::ratio<86'400>>(365.2425 / 50)));
  source().probes = {samsung(0, 301 * terabyte)};
  ASSERT_TRUE(poll().has_value());
  const Endurance& endurance = collector().latest()->drives[0].endurance;
  ASSERT_TRUE(endurance.years_left.has_value()) << endurance.unavailable;
  EXPECT_NEAR(*endurance.tb_per_year, 50.0, 0.01);
  EXPECT_NEAR(*endurance.used_percent, 301.0 / 12.0, 0.01);
  EXPECT_NEAR(*endurance.years_left, (1200.0 - 301.0) / 50.0, 0.01);
}

TEST_F(HealthCollectorTest, SourceFailureIsReported) {
  source().failure = Error{.code = Errc::not_found, .message = "no physical disk could be opened"};
  const Result<std::vector<telemetry::Measurement>> measured = poll();
  ASSERT_FALSE(measured.has_value());
  EXPECT_EQ(measured.error().message, "no physical disk could be opened");
}

TEST(HealthSchema, MigrationsAreAdditive) {
  const std::array sets{store::store_migrations(), health_migrations()};
  const Result<void> checked = store::check_additive(sets);
  EXPECT_TRUE(checked.has_value()) << checked.error().message;
}

}  // namespace
}  // namespace myclean::health
