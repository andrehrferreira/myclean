#include "core/resident/tracker.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/alerts/engine.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "core/resident/leak.hpp"
#include "core/resident/sampler.hpp"
#include "resident_fixtures.hpp"

namespace myclean::resident {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::noon;
using testing::view_of;

class TrackerTest : public testing::StoreTest {};

// A leaky program and a steady one, `minutes` after noon: the leaky one gains 2 MiB a minute.
telemetry::ProcessView machine(std::int64_t minutes) {
  const auto growth = static_cast<std::uint64_t>(minutes) * 2;
  return view_of(
      noon + std::chrono::minutes{minutes} + 30s,
      {{{.pid = 10, .image = "Leaky.exe", .private_bytes = Bytes{(500 + growth) << 20U}}},
       {{.pid = 20, .image = "steady.exe", .private_bytes = 300_MiB}}});
}

TEST_F(TrackerTest, ReportsLeakReadingsOfEveryProgramWhenABucketCloses) {
  Tracker tracker(store());
  std::optional<std::vector<alerts::Reading>> latest;
  for (std::int64_t minute = 0; minute <= std::int64_t{7} * 60; ++minute) {
    const Result<std::vector<alerts::Reading>> readings = tracker.tick(machine(minute), {});
    ASSERT_TRUE(readings.has_value()) << readings.error().message;
    // A minute is recorded once the next one began; a bucket closes every 5 minutes.
    EXPECT_EQ(readings->empty(), minute < 6 || minute % 5 != 1) << minute;
    if (!readings->empty()) {
      latest = *readings;
    }
  }
  ASSERT_TRUE(latest.has_value());
  const std::vector<alerts::Reading> last = latest.value_or(std::vector<alerts::Reading>{});
  ASSERT_EQ(last.size(), 2U);
  EXPECT_EQ(last[0].entity, "leaky.exe");
  EXPECT_GE(last[0].value, 6.0);
  EXPECT_EQ(last[1].entity, "steady.exe");
  EXPECT_EQ(last[1].value, 0.0);
}

TEST_F(TrackerTest, ReadsEachNetworkMinuteOnceItIsStored) {
  Tracker tracker(store());
  std::vector<std::pair<WallTime, WallTime>> asked;
  bool failing = false;
  const TrafficSource traffic = [&](WallTime from,
                                    WallTime to) -> Result<std::vector<ProcessTraffic>> {
    asked.emplace_back(from, to);
    if (failing) {
      return fail(Errc::io_error, "database is locked");
    }
    return std::vector<ProcessTraffic>{{.pid = 10, .image = "Leaky.exe", .bytes = 100}};
  };
  ASSERT_TRUE(tracker.tick(machine(0), traffic).has_value());
  EXPECT_TRUE(asked.empty());
  ASSERT_TRUE(tracker.tick(machine(1), traffic).has_value());
  ASSERT_EQ(asked.size(), 1U);
  EXPECT_EQ(asked[0], std::pair(noon - 2min, noon - 1min));

  failing = true;
  EXPECT_FALSE(tracker.tick(machine(2), traffic).has_value());
  failing = false;
  ASSERT_TRUE(tracker.tick(machine(3), traffic).has_value());
  ASSERT_EQ(asked.size(), 3U);
  // The minute that failed is read again with the next one.
  EXPECT_EQ(asked[2], std::pair(noon - 1min, noon + 1min));
}

TEST_F(TrackerTest, FlushRecordsTheMinuteObservedSoFar) {
  {
    Tracker tracker(store());
    ASSERT_TRUE(tracker.tick(machine(0), {}).has_value());
    ASSERT_TRUE(tracker.tick(machine(1), {}).has_value());
    tracker.flush();
    ASSERT_EQ(tracker.history().trends().at("leaky.exe").size(), 1U);
    EXPECT_EQ(tracker.history().trends().at("leaky.exe")[0].samples, 2);
  }
  reopen();
  Tracker restarted(store());
  ASSERT_TRUE(restarted.load(noon + 3min).has_value());
  ASSERT_EQ(restarted.history().trends().size(), 2U);
  EXPECT_EQ(restarted.history().trends().at("steady.exe")[0],
            (Point{.time = noon, .private_bytes = 300_MiB, .samples = 2}));
}

}  // namespace
}  // namespace myclean::resident
