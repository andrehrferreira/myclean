#include "core/tray_model/poller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/tray_model/replies.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;
using testing::FakeLink;
using testing::Json;

constexpr std::int64_t start = 1'000'000;

// The service as the fake plays it: metrics.latest reports `latest_time`; metrics.query
// answers one point per second up to `latest_time` (inclusive) with value 5.
class PollerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.on("metrics.latest",
             [this](const Json&) -> Result<Json> { return testing::latest_json(latest_time_); });
    link_.on("metrics.query", [this](const Json& params) -> Result<Json> {
      std::string entity = params.at("entity").get<std::string>();
      std::ranges::replace(entity, '_', ',');  // the service answers with the series entity
      const std::int64_t to = std::min(params.at("to").get<std::int64_t>(), latest_time_ + 1);
      return testing::query_json(params.at("metric").get<std::string>(), entity,
                                 params.at("from").get<std::int64_t>(), to, 5.0);
    });
    link_.on("processes.top", [](const Json&) -> Result<Json> { return testing::top_json(); });
  }

  PollResult poll_at(std::int64_t second, bool dashboard_open) {
    latest_time_ = second;
    return poller_.poll(from_unix_seconds(second), dashboard_open);
  }

  [[nodiscard]] FakeLink& link() { return link_; }

 private:
  FakeLink link_;
  Poller poller_{link_, 600s};
  std::int64_t latest_time_ = start;
};

TEST_F(PollerTest, UnreachableServiceStopsAfterTheFirstCall) {
  link().set_online(false);
  const PollResult result = poll_at(start, true);
  EXPECT_FALSE(result.reachable);
  EXPECT_NE(result.error.find("not running"), std::string::npos);
  EXPECT_FALSE(result.latest);
  EXPECT_EQ(link().calls().size(), 1U);
}

TEST_F(PollerTest, ClosedDashboardAsksOnlyForTheLatestMetrics) {
  const PollResult result = poll_at(start, false);
  EXPECT_TRUE(result.reachable);
  EXPECT_TRUE(result.error.empty());
  ASSERT_TRUE(result.latest.has_value());
  EXPECT_EQ((*result.latest).metrics.value("cpu.utility", "system"), 42.0);
  EXPECT_FALSE(result.top_by_cpu);
  EXPECT_TRUE(result.history.empty());
  ASSERT_EQ(link().calls().size(), 1U);
  EXPECT_EQ(link().calls()[0].method, "metrics.latest");
  EXPECT_EQ(link().calls()[0].params, Json::object());
}

TEST_F(PollerTest, OpenDashboardAlsoAsksForTopTreesByCpuAndByMemory) {
  const PollResult result = poll_at(start, true);
  const std::vector<FakeLink::Call> top = link().calls_of("processes.top");
  ASSERT_EQ(top.size(), 2U);
  EXPECT_EQ(top[0].params, (Json{{"sort", "cpu"}, {"limit", Poller::top_trees}}));
  EXPECT_EQ(top[1].params, (Json{{"sort", "memory"}, {"limit", Poller::top_trees}}));
  ASSERT_TRUE(result.top_by_cpu.has_value());
  ASSERT_TRUE(result.top_by_memory.has_value());
  EXPECT_EQ((*result.top_by_cpu).size(), 2U);
}

TEST_F(PollerTest, OpenDashboardFetchesEveryProcessorFromTheWindowStart) {
  const PollResult result = poll_at(start, true);
  const std::vector<FakeLink::Call> queries = link().calls_of("metrics.query");
  ASSERT_EQ(queries.size(), 8U);  // 4 logical processors x (utility, frequency)
  EXPECT_EQ(
      queries[0].params,
      (Json{
          {"metric", "cpu.utility"}, {"entity", "0_0"}, {"from", start - 600}, {"to", start + 1}}));
  EXPECT_EQ(queries[7].params.at("metric"), "cpu.frequency");
  EXPECT_EQ(queries[7].params.at("entity"), "0_3");
  ASSERT_EQ(result.history.size(), 8U);
  EXPECT_EQ(result.history[0].key, (SeriesKey{.metric = "cpu.utility", .entity = "0,0"}));
  EXPECT_EQ(result.history[0].samples.size(), 601U);
}

TEST_F(PollerTest, ProcessorsContinueWhereThePreviousPollLeftOff) {
  (void)poll_at(start, true);
  link().clear_calls();
  const PollResult result = poll_at(start + 1, true);
  const std::vector<FakeLink::Call> queries = link().calls_of("metrics.query");
  ASSERT_EQ(queries.size(), 8U);
  EXPECT_EQ(queries[0].params.at("from"), start + 1);
  EXPECT_EQ(queries[0].params.at("to"), start + 2);
  ASSERT_EQ(result.history.size(), 8U);
  ASSERT_EQ(result.history[0].samples.size(), 1U);
  EXPECT_EQ(result.history[0].samples[0].time, from_unix_seconds(start + 1));
}

TEST_F(PollerTest, ALatePollFetchesTheSamplesItMissed) {
  (void)poll_at(start, true);
  link().clear_calls();
  const PollResult result = poll_at(start + 3, true);
  const auto gap = std::ranges::find_if(result.history, [](const Series& series) {
    return series.key == SeriesKey{.metric = "cpu.utility", .entity = "system"};
  });
  ASSERT_NE(gap, result.history.end());
  ASSERT_EQ(gap->samples.size(), 2U);
  EXPECT_EQ(gap->samples[0].time, from_unix_seconds(start + 1));
  EXPECT_EQ(gap->samples[1].time, from_unix_seconds(start + 2));
  const std::vector<FakeLink::Call> queries = link().calls_of("metrics.query");
  EXPECT_TRUE(std::ranges::any_of(queries, [](const FakeLink::Call& call) {
    return call.params ==
           Json{{"metric", "disk.idle"}, {"entity", "10"}, {"from", start + 1}, {"to", start + 3}};
  }));
}

TEST_F(PollerTest, NoGapFillForConsecutiveSecondsOrWhileTheDashboardIsClosed) {
  (void)poll_at(start, false);
  (void)poll_at(start + 1, false);
  (void)poll_at(start + 5, false);
  EXPECT_TRUE(link().calls_of("metrics.query").empty());
  (void)poll_at(start + 6, true);
  // Only the 8 per-processor queries: start + 6 follows start + 5.
  EXPECT_EQ(link().calls_of("metrics.query").size(), 8U);
}

TEST_F(PollerTest, AMissingMethodLeavesItsPartUnavailable) {
  FakeLink bare;
  bare.on("metrics.latest",
          [](const Json&) -> Result<Json> { return testing::latest_json(start); });
  Poller poller(bare, 600s);
  const PollResult result = poller.poll(from_unix_seconds(start), true);
  EXPECT_TRUE(result.reachable);
  EXPECT_TRUE(result.latest);
  EXPECT_FALSE(result.top_by_cpu);
  EXPECT_FALSE(result.top_by_memory);
  EXPECT_TRUE(result.history.empty());
}

TEST_F(PollerTest, AMalformedReplyIsIgnored) {
  link().on("metrics.latest", [](const Json&) -> Result<Json> { return Json{{"time", "soon"}}; });
  const PollResult result = poll_at(start, false);
  EXPECT_TRUE(result.reachable);
  EXPECT_FALSE(result.latest);
}

TEST_F(PollerTest, AConnectionThatBreaksDuringThePollMakesItUnreachable) {
  link().on("processes.top", [this](const Json&) -> Result<Json> {
    link().set_online(false);
    return fail(Errc::invalid_state, "the pipe broke");
  });
  const PollResult result = poll_at(start, true);
  EXPECT_FALSE(result.reachable);
  EXPECT_FALSE(result.error.empty());
}

}  // namespace
}  // namespace myclean::tray_model
