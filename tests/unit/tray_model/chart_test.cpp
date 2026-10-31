#include "core/tray_model/chart.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;

Sample at(int second, double value) {
  return Sample{.time = WallTime{std::chrono::seconds{1'000'000 + second}}, .value = value};
}

std::vector<double> values(const ChartSeries& series) {
  return {series.values().begin(), series.values().end()};
}

TEST(ChartTest, TimeIsUnixSeconds) {
  EXPECT_DOUBLE_EQ(chart_time(WallTime{12s}), 12.0);
  EXPECT_DOUBLE_EQ(chart_time(WallTime{1500ms}), 1.5);
}

TEST(ChartTest, KeepsSamplesInTimeOrder) {
  ChartSeries series(60s);
  EXPECT_TRUE(series.add(at(1, 10.0)));
  EXPECT_TRUE(series.add(at(3, 30.0)));
  EXPECT_TRUE(series.add(at(2, 20.0)));  // a missed sample filled in later
  EXPECT_EQ(values(series), (std::vector<double>{10.0, 20.0, 30.0}));
  EXPECT_TRUE(std::ranges::is_sorted(series.times()));
  EXPECT_EQ(series.newest_value(), 30.0);
  EXPECT_DOUBLE_EQ(*series.newest_time(), chart_time(at(3, 0.0).time));
}

TEST(ChartTest, IgnoresASecondSampleOfTheSameInstant) {
  ChartSeries series(60s);
  EXPECT_TRUE(series.add(at(1, 10.0)));
  EXPECT_FALSE(series.add(at(1, 99.0)));
  EXPECT_EQ(values(series), std::vector<double>{10.0});
}

TEST(ChartTest, DropsWhatFallsOutOfTheWindow) {
  ChartSeries series(10s);
  for (int second = 0; second <= 15; ++second) {
    series.add(at(second, second));
  }
  EXPECT_EQ(series.times().size(), 11U);  // seconds 5 to 15
  EXPECT_EQ(series.values().front(), 5.0);
  EXPECT_FALSE(series.add(at(2, 2.0)));
}

TEST(ChartTest, EmptyChart) {
  const ChartSeries series(10s);
  EXPECT_TRUE(series.empty());
  EXPECT_FALSE(series.newest_time());
  EXPECT_FALSE(series.newest_value());
}

TEST(SnapshotTest, NaturalOrderComparesNumbersByValue) {
  EXPECT_TRUE(natural_less("2", "10"));
  EXPECT_FALSE(natural_less("10", "2"));
  EXPECT_TRUE(natural_less("0,9", "0,10"));
  EXPECT_TRUE(natural_less("0,63", "1,0"));
  EXPECT_TRUE(natural_less("C:", "D:"));
  EXPECT_FALSE(natural_less("7", "7"));
  EXPECT_TRUE(natural_less("disk", "disk2"));
}

TEST(SnapshotTest, ValuesAndEntities) {
  MetricsSnapshot snapshot;
  for (const char* disk : {"10", "2", "0"}) {
    snapshot.set(SeriesKey{.metric = "disk.idle", .entity = disk}, at(0, 1.0));
  }
  snapshot.set(SeriesKey{.metric = "disk.iops", .entity = "5"}, at(0, 7.0));
  EXPECT_EQ(snapshot.entities("disk.idle"), (std::vector<std::string>{"0", "2", "10"}));
  EXPECT_EQ(snapshot.value("disk.iops", "5"), 7.0);
  EXPECT_FALSE(snapshot.value("disk.iops", "0"));
  EXPECT_TRUE(snapshot.entities("cpu.utility").empty());
}

}  // namespace
}  // namespace myclean::tray_model
