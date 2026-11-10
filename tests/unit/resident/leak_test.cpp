#include "core/resident/leak.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/alerts/engine.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "resident_fixtures.hpp"

namespace myclean::resident {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::noon;

constexpr double mib = 1024.0 * 1024.0;

// 5-minute buckets over `span` ending at `end`, private bytes in MiB given by `at` (minutes
// before `end`).
std::vector<Point> trend(std::chrono::minutes span, WallTime end,
                         const std::function<double(std::int64_t minutes_before)>& at) {
  std::vector<Point> points;
  for (std::int64_t before = span.count(); before >= 0; before -= 5) {
    points.push_back(Point{.time = end - std::chrono::minutes{before},
                           .private_bytes = Bytes{static_cast<std::uint64_t>(at(before) * mib)},
                           .samples = 5});
  }
  return points;
}

// A little noise, the same in every run: garbage collection and caches.
double jitter(std::int64_t minutes) {
  return static_cast<double>((minutes * 7919) % 13) - 6.0;
}

TEST(SteadyGrowthTest, SixHoursOfSustainedGrowthIsALeak) {
  // Flat at 500 MiB, then 6 hours of +100 MiB per hour (with noise) up to now.
  const std::vector<Point> points = trend(10h, noon, [](std::int64_t before) {
    const double rising = before < 360 ? (360.0 - static_cast<double>(before)) * 100.0 / 60.0 : 0;
    return 500.0 + rising + jitter(before);
  });
  const std::optional<Growth> found = steady_growth(points, noon + 3min);
  ASSERT_TRUE(found.has_value());
  const Growth growth = found.value_or(Growth{});
  EXPECT_GE(growth.hours, 6);
  ASSERT_EQ(growth.hourly.size(), static_cast<std::size_t>(growth.hours));
  EXPECT_GT(growth.hourly.back().count(), growth.hourly.front().count() + (400 * (1ULL << 20U)));

  const alerts::Reading reading = leak_reading("armourycrate.service.exe", found);
  EXPECT_EQ(reading.metric, growth_metric);
  EXPECT_EQ(reading.entity, "armourycrate.service.exe");
  EXPECT_DOUBLE_EQ(reading.value, growth.hours);
  // The shipped rule fires above 5 hours.
  EXPECT_GT(reading.value, 5.0);
  EXPECT_NE(reading.detail.find("armourycrate.service.exe: private bytes rose from"),
            std::string::npos);
  EXPECT_NE(reading.detail.find("hourly medians: "), std::string::npos);
}

TEST(SteadyGrowthTest, ABuildSpikeThatFellBackWithinTheHourIsNoLeak) {
  // Flat at 800 MiB for 8 hours, with a 40-minute build that pushed it to 3 GiB 3 hours ago.
  const std::vector<Point> points = trend(
      8h, noon, [](std::int64_t before) { return before >= 160 && before < 200 ? 3072.0 : 800.0; });
  EXPECT_FALSE(steady_growth(points, noon).has_value());
  EXPECT_EQ(leak_reading("devenv.exe", steady_growth(points, noon)).value, 0.0);
}

TEST(SteadyGrowthTest, ABuildRunningNowIsNoLeak) {
  // Flat for 7 hours, then rising over the last 40 minutes.
  const std::vector<Point> points = trend(7h, noon, [](std::int64_t before) {
    return before < 40 ? 800.0 + (static_cast<double>(40 - before) * 50.0) : 800.0;
  });
  EXPECT_FALSE(steady_growth(points, noon).has_value());
}

TEST(SteadyGrowthTest, GrowthTooSmallToMatterIsNoLeak) {
  // 2 GiB growing by 8 MiB an hour for 8 hours: 64 MiB is under 10 % of its size.
  const std::vector<Point> points = trend(8h, noon, [](std::int64_t before) {
    return 2048.0 + (static_cast<double>(480 - before) * 8.0 / 60.0);
  });
  EXPECT_FALSE(steady_growth(points, noon).has_value());
}

TEST(SteadyGrowthTest, AProgramNoLongerRunningIsNoLeak) {
  const std::vector<Point> points =
      trend(8h, noon, [](std::int64_t before) { return 4000.0 - static_cast<double>(before); });
  EXPECT_TRUE(steady_growth(points, noon + 10min).has_value());
  EXPECT_FALSE(steady_growth(points, noon + stale_after + 1min).has_value());
}

TEST(SteadyGrowthTest, AnHourWithoutEnoughBucketsEndsTheRun) {
  // Steady growth for 8 hours, but MyClean was stopped for most of the hour 3 hours ago.
  std::vector<Point> points = trend(
      8h, noon, [](std::int64_t before) { return 6000.0 - (static_cast<double>(before) * 10); });
  std::erase_if(points, [](const Point& point) {
    return point.time > noon - 3h && point.time <= noon - 2h - 25min;
  });
  const std::optional<Growth> found = steady_growth(points, noon);
  ASSERT_TRUE(found.has_value());
  const Growth growth = found.value_or(Growth{});
  EXPECT_EQ(growth.hours, 2);
  EXPECT_LT(leak_reading("app.exe", found).value, 6.0);
}

TEST(SteadyGrowthTest, NoTrendNoReading) {
  EXPECT_FALSE(steady_growth({}, noon).has_value());
  const alerts::Reading reading = leak_reading("app.exe", std::nullopt);
  EXPECT_EQ(reading.metric, growth_metric);
  EXPECT_EQ(reading.value, 0.0);
  EXPECT_TRUE(reading.detail.empty());
}

}  // namespace
}  // namespace myclean::resident
