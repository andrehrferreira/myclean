#include "core/tray_model/poller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/input/incident.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/replies.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;
using testing::FakeLink;
using testing::Json;

constexpr std::int64_t start = 1'000'000;

// The service as the fake plays it: metrics.latest reports `latest_time`; metrics.query
// answers one point per second up to `latest_time` (inclusive) with value 5, under the series
// entity, merged into max_points points when asked; alerts.badge reports `badge`; every other
// method answers its fixture.
class PollerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.on("metrics.latest",
             [this](const Json&) -> Result<Json> { return testing::latest_json(latest_time_); });
    link_.on("metrics.query", [this](const Json& params) -> Result<Json> {
      std::string entity = params.at("entity").get<std::string>();
      std::ranges::replace(entity, '_', ',');  // the service answers with the series entity
      const std::int64_t from = params.at("from").get<std::int64_t>();
      const std::int64_t to = std::min(params.at("to").get<std::int64_t>(), latest_time_ + 1);
      const std::int64_t points = std::max<std::int64_t>(1, params.value("max_points", to - from));
      return testing::query_json(params.at("metric").get<std::string>(), entity, from, to, 5.0,
                                 std::max<std::int64_t>(1, (to - from) / points));
    });
    link_.on("processes.top", [](const Json&) -> Result<Json> { return testing::top_json(); });
    link_.on("alerts.badge", [this](const Json&) -> Result<Json> { return badge_; });
    link_.on("alerts.list", [](const Json&) -> Result<Json> {
      return testing::alerts_json(
          Json::array({testing::alert_json("volume.fill-up", "C", "warning", true, start - 10)}));
    });
    link_.on("disks.health", [](const Json&) -> Result<Json> { return testing::health_json(); });
    link_.on("input.summary", [](const Json&) -> Result<Json> { return testing::summary_json(); });
  }

  PollResult poll_at(std::int64_t second, bool dashboard_open,
                     const std::vector<Request>& requests = {}) {
    latest_time_ = second;
    return poller_.poll(from_unix_seconds(second), dashboard_open, requests);
  }

  [[nodiscard]] FakeLink& link() { return link_; }
  void set_badge(int count, std::string_view worst) { badge_ = testing::badge_json(count, worst); }

 private:
  FakeLink link_;
  Poller poller_{link_, 600s};
  std::int64_t latest_time_ = start;
  Json badge_ = testing::badge_json(0, "none");
};

std::vector<std::string> methods(const std::vector<FakeLink::Call>& calls) {
  std::vector<std::string> names;
  names.reserve(calls.size());
  for (const FakeLink::Call& call : calls) {
    names.push_back(call.method);
  }
  return names;
}

TEST_F(PollerTest, UnreachableServiceStopsAfterTheFirstCall) {
  link().set_online(false);
  const PollResult result = poll_at(start, true);
  EXPECT_FALSE(result.reachable);
  EXPECT_NE(result.error.find("not running"), std::string::npos);
  EXPECT_FALSE(result.latest);
  EXPECT_EQ(link().calls().size(), 1U);
}

TEST_F(PollerTest, ClosedDashboardAsksForMetricsAndBadgeAndFirstForHealthAlertsAndPlans) {
  const PollResult first = poll_at(start, false);
  EXPECT_TRUE(first.reachable);
  EXPECT_TRUE(first.error.empty());
  ASSERT_TRUE(first.latest.has_value());
  EXPECT_EQ((*first.latest).metrics.value("cpu.utility", "system"), 42.0);
  EXPECT_FALSE(first.top_by_cpu);
  EXPECT_FALSE(first.freezes);
  EXPECT_TRUE(first.history.empty());
  EXPECT_EQ(
      methods(link().calls()),
      (std::vector<std::string>{"metrics.latest", "alerts.badge", "disks.health", "alerts.list",
                                "governor.status", "cleaning.status", "plans.list"}));
  EXPECT_EQ(link().calls()[0].params, Json::object());
  EXPECT_EQ(link().calls()[3].params, (Json{{"since", start - (std::int64_t{30} * 86'400)}}));
  ASSERT_TRUE(first.drives.has_value());
  EXPECT_EQ((*first.drives).size(), 3U);
  ASSERT_TRUE(first.alerts.has_value());
  EXPECT_EQ((*first.alerts).size(), 1U);
  EXPECT_EQ(first.badge, (AlertSummary{.active = 0, .worst = std::nullopt}));

  link().clear_calls();
  const PollResult second = poll_at(start + 1, false);
  EXPECT_EQ(methods(link().calls()),
            (std::vector<std::string>{"metrics.latest", "alerts.badge", "governor.status"}));
  EXPECT_FALSE(second.drives);
  EXPECT_FALSE(second.alerts);
  EXPECT_TRUE(second.badge);
}

TEST_F(PollerTest, HealthAndAlertsEveryMinuteAndAtOnceAfterReconnecting) {
  (void)poll_at(start, false);
  link().clear_calls();
  (void)poll_at(start + 59, false);
  EXPECT_TRUE(link().calls_of("disks.health").empty());
  EXPECT_TRUE(poll_at(start + 60, false).drives);
  EXPECT_EQ(link().calls_of("alerts.list").size(), 1U);

  link().set_online(false);
  EXPECT_FALSE(poll_at(start + 61, false).reachable);
  link().set_online(true);
  link().clear_calls();
  const PollResult back = poll_at(start + 62, false);
  EXPECT_TRUE(back.drives);
  EXPECT_TRUE(back.alerts);
}

TEST_F(PollerTest, TheAlertListFollowsBadgeChanges) {
  (void)poll_at(start, false);
  link().clear_calls();
  set_badge(1, "warning");
  const PollResult changed = poll_at(start + 1, false);
  EXPECT_EQ(changed.badge, (AlertSummary{.active = 1, .worst = AlertSeverity::warning}));
  EXPECT_TRUE(changed.alerts);
  EXPECT_FALSE(changed.drives);
  EXPECT_FALSE(poll_at(start + 2, false).alerts) << "the badge did not change";
}

TEST_F(PollerTest, OpenDashboardAlsoAsksForTopTreesAndFreezes) {
  const PollResult result = poll_at(start, true);
  const std::vector<FakeLink::Call> top = link().calls_of("processes.top");
  ASSERT_EQ(top.size(), 2U);
  EXPECT_EQ(top[0].params, (Json{{"sort", "cpu"}, {"limit", Poller::top_trees}}));
  EXPECT_EQ(top[1].params, (Json{{"sort", "memory"}, {"limit", Poller::top_trees}}));
  ASSERT_TRUE(result.top_by_cpu.has_value());
  ASSERT_TRUE(result.top_by_memory.has_value());
  EXPECT_EQ((*result.top_by_cpu).size(), 2U);
  ASSERT_TRUE(result.freezes.has_value());
  EXPECT_EQ((*result.freezes).size(), 2U);
  EXPECT_TRUE(link().calls_of("metrics.query").empty())
      << "the processors come with metrics.latest";
}

TEST_F(PollerTest, OpeningTheDashboardFetchesHealthAlertsAndFreezesAtOnce) {
  (void)poll_at(start, false);
  link().clear_calls();
  const PollResult opened = poll_at(start + 1, true);
  EXPECT_TRUE(opened.drives);
  EXPECT_TRUE(opened.alerts);
  EXPECT_TRUE(opened.freezes);
  link().clear_calls();
  const PollResult next = poll_at(start + 2, true);
  EXPECT_FALSE(next.freezes);
  EXPECT_TRUE(next.top_by_cpu);
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

TEST_F(PollerTest, MissedProcessorSamplesKeepTheTokenEntity) {
  (void)poll_at(start, true);
  const PollResult result = poll_at(start + 3, true);
  const auto core = std::ranges::find_if(result.history, [](const Series& series) {
    return series.key == SeriesKey{.metric = "cpu.utility", .entity = "0_1"};
  });
  ASSERT_NE(core, result.history.end()) << "the service answers 0,1; the tray knows it as 0_1";
  EXPECT_EQ(core->samples.size(), 2U);
}

TEST_F(PollerTest, NoGapFillForConsecutiveSecondsOrWhileTheDashboardIsClosed) {
  (void)poll_at(start, false);
  (void)poll_at(start + 1, false);
  (void)poll_at(start + 5, false);
  EXPECT_TRUE(link().calls_of("metrics.query").empty());
  (void)poll_at(start + 6, true);
  EXPECT_TRUE(link().calls_of("metrics.query").empty());
}

TEST_F(PollerTest, APastRangeQueriesTheChartedSeriesWithAPointLimit) {
  (void)poll_at(start, true);
  link().clear_calls();
  const PollResult result = poll_at(start + 1, true, {SelectRange{.range = Range::hour}});
  ASSERT_TRUE(result.range.has_value());
  EXPECT_EQ((*result.range).range, Range::hour);
  EXPECT_EQ((*result.range).time, from_unix_seconds(start + 1));
  const std::vector<FakeLink::Call> queries = link().calls_of("metrics.query");
  // cpu utility, DPC and interrupts, 7 memory lines, the GPU adapter, 7 series of 3 disks.
  EXPECT_EQ(queries.size(), 3U + 7U + 1U + (7U * 3U));
  for (const FakeLink::Call& query : queries) {
    EXPECT_EQ(query.params.at("from"), start + 1 - 3600);
    EXPECT_EQ(query.params.at("to"), start + 2);
    EXPECT_EQ(query.params.at("max_points"), Poller::range_points);
    EXPECT_NE(query.params.at("entity"), "0_0") << "per-processor charts are live only";
    EXPECT_NE(query.params.at("metric"), "process.count");
  }
  EXPECT_EQ((*result.range).series.size(), queries.size());

  link().clear_calls();
  EXPECT_FALSE(poll_at(start + 2, true).range);
  EXPECT_TRUE(poll_at(start + 61, true).range) << "refreshed every minute";
  EXPECT_TRUE(poll_at(start + 62, true, {SelectRange{.range = Range::month}}).range);
}

TEST_F(PollerTest, ThePastRangeIsFetchedOnlyWhileTheDashboardIsOpen) {
  (void)poll_at(start, false, {SelectRange{.range = Range::day}});
  EXPECT_TRUE(link().calls_of("metrics.query").empty());
  // Opening the dashboard starts on the live range.
  EXPECT_FALSE(poll_at(start + 1, true).range);
  EXPECT_TRUE(poll_at(start + 2, true, {SelectRange{.range = Range::day}}).range);
  EXPECT_FALSE(poll_at(start + 3, true, {SelectRange{.range = Range::live}}).range);
}

TEST_F(PollerTest, SnoozeAndDismissAreSentAndRefreshTheAlerts) {
  link().on("alerts.snooze",
            [](const Json&) -> Result<Json> { return Json{{"snoozed_until", start + 3600}}; });
  (void)poll_at(start, false);
  link().clear_calls();
  const PollResult snoozed = poll_at(
      start + 1, false,
      {SnoozeAlert{.rule = "volume.fill-up", .entity = "C", .duration = std::chrono::hours{1}}});
  const std::vector<FakeLink::Call> calls = link().calls_of("alerts.snooze");
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls[0].params, (Json{{"rule", "volume.fill-up"}, {"entity", "C"}, {"minutes", 60}}));
  EXPECT_EQ(snoozed.alert_action, "");
  EXPECT_TRUE(snoozed.alerts) << "the list is fetched again after the change";

  const PollResult dismissed =
      poll_at(start + 2, false, {DismissAlert{.rule = "volume.fill-up", .entity = "C"}});
  ASSERT_TRUE(dismissed.alert_action.has_value());
  EXPECT_TRUE((*dismissed.alert_action).starts_with("Dismiss failed: "));
  EXPECT_NE((*dismissed.alert_action).find("method not found"), std::string::npos);
}

TEST_F(PollerTest, ExplainAnswersOrSaysWhyNot) {
  link().on("processes.explain", [](const Json& params) -> Result<Json> {
    if (params.at("pid") != 101) {
      return fail(Errc::not_found, "process 999 was not found");
    }
    return testing::explain_json();
  });
  const PollResult found = poll_at(start, true, {ExplainProcess{.pid = 101}});
  ASSERT_TRUE(found.explanation.has_value());
  ASSERT_TRUE((*found.explanation).has_value());
  EXPECT_EQ((*found.explanation).value().image, "browser.exe");
  const PollResult missing = poll_at(start + 1, true, {ExplainProcess{.pid = 999}});
  ASSERT_TRUE(missing.explanation.has_value());
  ASSERT_FALSE((*missing.explanation).has_value());
  EXPECT_EQ((*missing.explanation).error().message, "process 999 was not found");
  EXPECT_FALSE(poll_at(start + 2, true).explanation);
}

TEST_F(PollerTest, IncidentsAreForwardedAndRefreshTheFreezes) {
  std::vector<Json> recorded;
  link().on("input.record_incident", [&recorded](const Json& params) -> Result<Json> {
    recorded.push_back(params.at("incident"));
    return Json{{"id", static_cast<std::int64_t>(recorded.size())}};
  });
  IncidentQueue queue;
  Poller poller(link(), 600s, &queue);
  (void)poller.poll(from_unix_seconds(start), true);
  queue.record(input::Incident{
      .chord = input::Chord::paste, .time = from_unix_seconds(start), .latency = 600ms});
  link().clear_calls();
  const PollResult result = poller.poll(from_unix_seconds(start + 1), true);
  ASSERT_EQ(recorded.size(), 1U);
  EXPECT_EQ(recorded[0].at("chord"), "paste");
  EXPECT_EQ(recorded[0].at("latency_ms"), 600);
  EXPECT_TRUE(result.freezes) << "a new incident refreshes the Freezes view";
  ASSERT_TRUE(result.incidents.has_value());
  EXPECT_EQ((*result.incidents).sent, 1U);
  EXPECT_EQ((*result.incidents).queued, 0U);
}

// Governor spec, "Rate-Limited Owner Notifications", "Pause Governor", "Game Mode": the status at
// every poll, after the owner's requests.
TEST_F(PollerTest, GovernorStatusEveryPollAndTheOwnersRequestsBeforeIt) {
  link().on("governor.status", [](const Json&) -> Result<Json> {
    return testing::governor_status_json(
        Json::array({testing::governor_notification_json(1, start, 300, "bigjob.exe", "suspend")}));
  });
  link().on("governor.choose", [](const Json&) -> Result<Json> { return Json{{"commands", 2}}; });
  link().on("governor.pause",
            [](const Json&) -> Result<Json> { return Json{{"paused_until", start + 3600}}; });
  const PollResult first = poll_at(start, false);
  ASSERT_TRUE(first.governor.has_value());
  EXPECT_EQ((*first.governor).notifications.size(), 1U);
  EXPECT_FALSE(first.governor_action);

  link().clear_calls();
  const PollResult chosen = poll_at(
      start + 1, false,
      {ChooseForTree{.root = GovernorProcess{.pid = 300, .created = 300000}, .choice = "resume"},
       PauseGovernor{.minutes = std::chrono::minutes{60}}, SetGameMode{.value = "off"}});
  EXPECT_EQ(methods(link().calls()),
            (std::vector<std::string>{"metrics.latest", "governor.choose", "governor.pause",
                                      "governor.game_mode", "alerts.badge", "governor.status"}));
  EXPECT_EQ(link().calls_of("governor.choose")[0].params,
            (Json{{"pid", 300}, {"created", 300000}, {"choice", "resume"}}));
  EXPECT_EQ(link().calls_of("governor.pause")[0].params, (Json{{"minutes", 60}}));
  EXPECT_EQ(link().calls_of("governor.game_mode")[0].params, (Json{{"override", "off"}}));
  ASSERT_TRUE(chosen.governor_action.has_value());
  EXPECT_TRUE((*chosen.governor_action).starts_with("Setting game mode failed: "))
      << "the last request's outcome; governor.game_mode is not offered by this fake";

  const PollResult ended =
      poll_at(start + 2, false, {PauseGovernor{.minutes = std::chrono::minutes{0}}});
  EXPECT_EQ(ended.governor_action, "");
  EXPECT_EQ(link().calls_of("governor.pause").back().params, (Json{{"minutes", 0}}));
}

TEST_F(PollerTest, OrphanDecisionsAreSent) {
  link().on("governor.orphans", [](const Json&) -> Result<Json> { return Json{{"trees", 88}}; });
  const PollResult ended = poll_at(start, false, {DecideOrphans{.id = 2, .decision = "always"}});
  EXPECT_EQ(link().calls_of("governor.orphans")[0].params,
            (Json{{"id", 2}, {"decision", "always"}}));
  EXPECT_EQ(ended.governor_action, "");
  link().on("governor.orphans", [](const Json&) -> Result<Json> {
    return fail(Errc::not_found, "no orphaned-loop rule 9");
  });
  const PollResult failed =
      poll_at(start + 1, false, {DecideOrphans{.id = 9, .decision = "forget"}});
  ASSERT_TRUE(failed.governor_action.has_value());
  EXPECT_TRUE((*failed.governor_action).starts_with("Forgetting the rule failed: "));
}

TEST_F(PollerTest, TheOwnersSessionStateIsSentEveryPollWhenItCanBeRead) {
  std::optional<UserStateReading> reading =
      UserStateReading{.state = "running_d3d_full_screen", .foreground_pid = 900};
  Poller poller(link(), 600s, nullptr, nullptr,
                [&reading]() -> std::optional<UserStateReading> { return reading; });
  (void)poller.poll(from_unix_seconds(start), false);
  (void)poller.poll(from_unix_seconds(start + 1), false);
  const std::vector<FakeLink::Call> sent = link().calls_of("governor.user_state");
  ASSERT_EQ(sent.size(), 2U);
  EXPECT_EQ(sent[0].params, (Json{{"state", "running_d3d_full_screen"}, {"foreground_pid", 900}}));

  reading.reset();
  (void)poller.poll(from_unix_seconds(start + 2), false);
  EXPECT_EQ(link().calls_of("governor.user_state").size(), 2U) << "nothing read, nothing sent";
  link().set_online(false);
  reading = UserStateReading{.state = "busy", .foreground_pid = 1};
  link().clear_calls();
  (void)poller.poll(from_unix_seconds(start + 3), false);
  EXPECT_TRUE(link().calls_of("governor.user_state").empty()) << "not while unreachable";
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
  EXPECT_FALSE(result.badge);
  EXPECT_FALSE(result.alerts);
  EXPECT_FALSE(result.drives);
  EXPECT_FALSE(result.freezes);
  EXPECT_FALSE(result.governor);
  EXPECT_TRUE(result.history.empty());
}

TEST_F(PollerTest, AMalformedReplyIsIgnored) {
  link().on("metrics.latest", [](const Json&) -> Result<Json> { return Json{{"time", "soon"}}; });
  link().on("disks.health", [](const Json&) -> Result<Json> { return Json{{"drives", 1}}; });
  const PollResult result = poll_at(start, false);
  EXPECT_TRUE(result.reachable);
  EXPECT_FALSE(result.latest);
  EXPECT_FALSE(result.drives);
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
