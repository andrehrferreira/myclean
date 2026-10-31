#include "core/telemetry/history.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/time_series.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "core/telemetry/volume_info.hpp"
#include "fake_sources.hpp"
#include "fakes/fake_clock.hpp"
#include "temp_store.hpp"

namespace myclean::telemetry {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime start = std::chrono::sys_days{std::chrono::October / 1 / 2026};
constexpr double gigabyte = 1.0e9;

// Hourly points of free space starting at `first` bytes and changing by `per_day` bytes a day.
std::vector<store::Aggregate> hourly(int hours, double first, double per_day) {
  std::vector<store::Aggregate> points;
  for (int hour = 0; hour < hours; ++hour) {
    const double value = first + (per_day * hour / 24.0);
    points.push_back({.time = start + std::chrono::hours{hour},
                      .min = value,
                      .max = value,
                      .mean = value,
                      .count = 60});
  }
  return points;
}

TEST(ProjectFill, AVolumeLosingSpaceSteadilyGetsDaysUntilFull) {
  // 100 GB free, losing 10 GB a day for a week: 30.4 GB left at the last point (hour 167).
  const FillProjection projection = project_fill(hourly(7 * 24, 100 * gigabyte, -10 * gigabyte));
  EXPECT_EQ(projection.state, FillState::filling);
  EXPECT_NEAR(projection.bytes_per_day, -10 * gigabyte, 1.0);
  ASSERT_TRUE(projection.days_until_full.has_value());
  EXPECT_NEAR(*projection.days_until_full, (100.0 - (10.0 * 167 / 24)) / 10.0, 1e-9);
}

TEST(ProjectFill, AVolumeWhoseFreeSpaceDidNotDecreaseIsNotFilling) {
  for (const double per_day : {0.0, 5 * gigabyte}) {
    const FillProjection projection = project_fill(hourly(7 * 24, 100 * gigabyte, per_day));
    EXPECT_EQ(projection.state, FillState::not_filling);
    EXPECT_FALSE(projection.days_until_full.has_value());
  }
}

TEST(ProjectFill, TooLittleHistoryGivesNoTrend) {
  EXPECT_EQ(project_fill({}).state, FillState::insufficient_data);
  EXPECT_EQ(project_fill(hourly(1, gigabyte, -gigabyte)).state, FillState::insufficient_data);
  std::vector<store::Aggregate> half_hour = hourly(2, gigabyte, -gigabyte);
  half_hour[1].time = start + 30min;
  EXPECT_EQ(project_fill(half_hour).state, FillState::insufficient_data);
}

TEST(VolumeEntity, IsTheFirstMountPointWithoutItsTrailingBackslash) {
  EXPECT_EQ(volume_entity({.id = R"(\\?\Volume{1}\)", .mount_points = {R"(C:\)"}}), "C:");
  EXPECT_EQ(volume_entity({.id = R"(\\?\Volume{2}\)", .mount_points = {R"(D:\mnt\data\)", "E:\\"}}),
            R"(D:\mnt\data)");
}

class HistoryTest : public ::testing::Test {
 protected:
  // Records C: free space losing 10 GB a day, one sample an hour for 7 days, then 10 minutes
  // of 1 s samples.
  void record_week() {
    const SeriesKey key{.metric = std::string(metric::volume_free), .entity = "C:"};
    for (int hour = 0; hour < 7 * 24; ++hour) {
      store_.get().record(key, Sample{.time = clock_.now(),
                                      .value = (100 * gigabyte) - (10 * gigabyte * hour / 24.0)});
      clock_.advance(1h);
      ASSERT_TRUE(store_.get().tick().has_value());
    }
    for (int second = 0; second < 600; ++second) {
      store_.get().record(key, Sample{.time = clock_.now(), .value = 30 * gigabyte});
      clock_.advance(1s);
    }
    ASSERT_TRUE(store_.get().tick().has_value());
  }

  fakes::FakeClock& clock() { return clock_; }
  store::Store& store() { return store_.get(); }

 private:
  fakes::FakeClock clock_{start};
  testing::TempStore store_{clock_};
};

TEST_F(HistoryTest, TheFinestResolutionTheRetentionCoversIsChosen) {
  record_week();
  const SeriesKey key{.metric = std::string(metric::volume_free), .entity = "C:"};
  const WallTime now = clock().now();

  const Result<History> recent = query_history(store(), key, now - 5min, now, now);
  ASSERT_TRUE(recent.has_value());
  EXPECT_EQ(recent->resolution, Resolution::raw);
  EXPECT_EQ(recent->points.size(), 300U);
  EXPECT_EQ(recent->points.front().count, 1);

  const Result<History> week = query_history(store(), key, now - std::chrono::days{7}, now, now);
  ASSERT_TRUE(week.has_value());
  EXPECT_EQ(week->resolution, Resolution::minute);
  EXPECT_GE(week->points.size(), 7U * 24U);

  const Result<History> half_year =
      query_history(store(), key, now - std::chrono::days{182}, now, now);
  ASSERT_TRUE(half_year.has_value());
  EXPECT_EQ(half_year->resolution, Resolution::hour);
  EXPECT_GE(half_year->points.size(), (7U * 24U) - 1U);
  EXPECT_EQ(to_string(half_year->resolution), "hour");
}

TEST_F(HistoryTest, AFillingVolumeIsProjectedFromItsStoredHistoryAndMappedToItsDisk) {
  record_week();
  const Result<FillProjection> projection = project_volume_fill(store(), "C:", clock().now());
  ASSERT_TRUE(projection.has_value());
  EXPECT_EQ(projection->state, FillState::filling);
  ASSERT_TRUE(projection->days_until_full.has_value());
  EXPECT_GT(*projection->days_until_full, 2.0);
  EXPECT_LT(*projection->days_until_full, 4.0);

  const testing::FakeVolumeInfo volumes{std::vector<VolumeStatus>{{.id = R"(\\?\Volume{c}\)",
                                                                   .mount_points = {R"(C:\)"},
                                                                   .size = 1000_GiB,
                                                                   .free = 30_GiB,
                                                                   .disks = {2}}}};
  VolumeCollector collector{volumes};
  EXPECT_EQ(collector.interval(), 60s);
  const Result<std::vector<Measurement>> measured = collector.collect(clock().now(), MonoTime{});
  ASSERT_TRUE(measured.has_value());
  ASSERT_EQ(measured->size(), 2U);
  EXPECT_EQ((*measured)[0].key.metric, metric::volume_size);
  EXPECT_EQ((*measured)[1].key.entity, "C:");
  EXPECT_EQ((*measured)[1].value, static_cast<double>((30_GiB).count()));
  ASSERT_NE(collector.latest(), nullptr);
  EXPECT_EQ(collector.latest()->volumes.at(0).disks, std::vector<std::uint32_t>{2});
}

}  // namespace
}  // namespace myclean::telemetry
