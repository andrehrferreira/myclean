#include "core/store/time_series.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "store_test_support.hpp"

namespace myclean::store {
namespace {

using namespace std::chrono_literals;
using testing::noon;
using testing::ok;
using testing::TempDir;

const SeriesKey core3{.metric = "cpu.core", .entity = "3"};

class TimeSeriesTest : public ::testing::Test {
 protected:
  void SetUp() override { open(); }

  void open() {
    store_.reset();
    Result<std::unique_ptr<Store>> opened = Store::open(dir().database(), clock());
    ASSERT_TRUE(ok(opened));
    store_ = std::move(*opened);
    ASSERT_TRUE(ok(store().tick()));
  }

  // One sample per second for `count` seconds, as the 1 s sampler would deliver them.
  void sample_every_second(int count, double (*value)(int second)) {
    for (int second = 0; second < count; ++second) {
      store().record(core3, {.time = clock().now(), .value = value(second)});
      clock().advance(1s);
      ASSERT_TRUE(ok(store().tick()));
    }
  }

  Store& store() { return *store_; }
  [[nodiscard]] const TempDir& dir() const { return dir_; }
  fakes::FakeClock& clock() { return clock_; }

 private:
  TempDir dir_;
  fakes::FakeClock clock_{noon + 5min};
  std::unique_ptr<Store> store_;
};

TEST_F(TimeSeriesTest, MinuteAggregateFromRawSamples) {
  const WallTime minute = clock().now();
  sample_every_second(60, [](int second) { return 10.0 + second; });
  clock().advance(Store::close_delay);  // the minute closes
  ASSERT_TRUE(ok(store().tick()));
  ASSERT_TRUE(ok(store().flush()));

  const Result<std::vector<Aggregate>> rows = store().minutes(core3, minute, minute + 1h);
  ASSERT_TRUE(ok(rows));
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front().time, minute);
  EXPECT_EQ(rows->front().min, 10.0);
  EXPECT_EQ(rows->front().max, 69.0);
  EXPECT_DOUBLE_EQ(rows->front().mean, 39.5);
  EXPECT_EQ(rows->front().count, 60);
}

TEST_F(TimeSeriesTest, HourAggregateFromMinuteAggregates) {
  clock().set_wall(noon + 1h);
  const WallTime hour = clock().now();
  sample_every_second(3600, [](int second) {
    const int minute = second / 60;
    const int within = second % 60;
    return (2.0 * minute) + (0.25 * within);
  });
  clock().advance(Store::close_delay);  // the hour closes
  ASSERT_TRUE(ok(store().tick()));

  const Result<std::vector<Aggregate>> minutes = store().minutes(core3, hour, hour + 1h);
  const Result<std::vector<Aggregate>> hours = store().hours(core3, hour, hour + 1h);
  ASSERT_TRUE(ok(minutes));
  ASSERT_TRUE(ok(hours));
  ASSERT_EQ(minutes->size(), 60U);
  ASSERT_EQ(hours->size(), 1U);
  const Aggregate& aggregate = hours->front();
  EXPECT_EQ(aggregate.time, hour);
  EXPECT_EQ(aggregate.min, std::ranges::min(*minutes, {}, &Aggregate::min).min);
  EXPECT_EQ(aggregate.max, std::ranges::max(*minutes, {}, &Aggregate::max).max);
  const double mean_of_minutes =
      std::accumulate(minutes->begin(), minutes->end(), 0.0,
                      [](double sum, const Aggregate& row) { return sum + row.mean; }) /
      60.0;
  EXPECT_DOUBLE_EQ(aggregate.mean, mean_of_minutes);
  EXPECT_EQ(aggregate.count, 3600);
}

TEST_F(TimeSeriesTest, HoursMissedWhileStoppedAreAggregatedAtTheNextStart) {
  const WallTime hour = noon - 3h;
  ASSERT_TRUE(
      ok(testing::exec(store(),
                       "INSERT INTO series(id, metric, entity) VALUES(7, 'cpu.core', '3');"
                       "INSERT INTO ts_minute(series, time, min, max, mean, count) VALUES"
                       "(7, " +
                           std::to_string(to_unix_seconds(hour)) +
                           ", 1, 5, 3, 60),"
                           "(7, " +
                           std::to_string(to_unix_seconds(hour + 1min)) + ", 2, 8, 5, 60);")));

  clock().set_wall(noon + 3h);
  open();

  const Result<std::vector<Aggregate>> hours = store().hours(core3, hour, noon + 3h);
  ASSERT_TRUE(ok(hours));
  ASSERT_EQ(hours->size(), 1U);
  EXPECT_EQ(hours->front().time, hour);
  EXPECT_EQ(hours->front().min, 1.0);
  EXPECT_EQ(hours->front().max, 8.0);
  EXPECT_EQ(hours->front().mean, 4.0);
  EXPECT_EQ(hours->front().count, 120);
}

TEST_F(TimeSeriesTest, RingHoldsOnlyTheLastHour) {
  sample_every_second(90 * 60, [](int second) { return static_cast<double>(second); });
  const WallTime last = clock().now() - 1s;  // the newest sample is one second old

  const std::vector<Sample> samples = store().raw(core3);

  // Every sample newer than one hour ago: the newest 3599, as the next one is not taken yet.
  ASSERT_EQ(samples.size(), 3599U);
  EXPECT_EQ(samples.front().time, clock().now() - 1h + 1s);
  EXPECT_EQ(samples.back().time, last);
  EXPECT_EQ(samples.back().value, (90 * 60) - 1);
}

TEST(SeriesBuffer, LateSampleOfAClosedMinuteReachesTheRingOnly) {
  SeriesBuffer buffer;
  const WallTime minute = noon;
  buffer.add(core3, {.time = minute + 10s, .value = 1.0});
  buffer.add(core3, {.time = minute + 1min, .value = 2.0});   // closes the first minute
  buffer.add(core3, {.time = minute + 20s, .value = 100.0});  // late

  const std::vector<ClosedMinute> closed = buffer.take_closed(minute);
  ASSERT_EQ(closed.size(), 1U);
  EXPECT_EQ(closed.front().aggregate.time, minute);
  EXPECT_EQ(closed.front().aggregate.max, 1.0);
  EXPECT_EQ(closed.front().aggregate.count, 1);

  const std::vector<Sample> raw = buffer.raw(core3, minute + 2min);
  ASSERT_EQ(raw.size(), 3U);
  EXPECT_EQ(raw[1].value, 100.0);  // kept in time order
}

TEST(SeriesBuffer, SeriesWithoutRecentSamplesAreEvicted) {
  SeriesBuffer buffer;
  buffer.add(core3, {.time = noon, .value = 1.0});
  static_cast<void>(buffer.take_closed(noon + 1min));
  buffer.evict(noon + 2h);
  EXPECT_TRUE(buffer.raw(core3, noon).empty());
}

}  // namespace
}  // namespace myclean::store
