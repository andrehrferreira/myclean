#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "inbox_fixtures.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using namespace std::chrono_literals;
using testing::FakeLink;
using testing::Json;

constexpr std::int64_t start = 1'000'000;

// A runner that records what it is given; the test finishes tasks by hand.
class FakeRunner final : public ITaskRunner {
 public:
  [[nodiscard]] bool idle() const override { return !running && !finished; }
  void start(Json task) override {
    started.push_back(std::move(task));
    running = true;
  }
  [[nodiscard]] std::optional<Report> report() const override { return finished; }
  void delivered() override {
    finished.reset();
    ++deliveries;
  }
  [[nodiscard]] std::string activity() const override { return running ? "running" : ""; }

  void finish(std::int64_t plan) {
    running = false;
    finished = Report{.plan = plan,
                      .result = Json{{"state", "verified"}, {"error", nullptr}},
                      .summary = "Plan " + std::to_string(plan) + ": done"};
  }

  bool running = false;
  std::optional<Report> finished;
  std::vector<Json> started;
  int deliveries = 0;
};

class InboxPollerTest : public ::testing::Test {
 protected:
  InboxPollerTest() {
    link_.on("metrics.latest",
             [](const Json&) -> Result<Json> { return testing::latest_json(start); });
    link_.on("alerts.badge",
             [](const Json&) -> Result<Json> { return testing::badge_json(0, "none"); });
    link_.on("cleaning.status", [this](const Json&) -> Result<Json> { return status_; });
    link_.on("plans.list", [](const Json&) -> Result<Json> { return testing::plans_json(); });
    link_.on("plans.get", [](const Json& params) -> Result<Json> {
      return testing::plan_get_json(params.at("id").get<std::int64_t>());
    });
    link_.on("quarantine.list",
             [](const Json&) -> Result<Json> { return testing::quarantine_json(); });
    link_.on("user_tasks.next", [this](const Json&) -> Result<Json> {
      return Json{{"task", std::exchange(task_, nullptr)}};
    });
    link_.on("user_tasks.report",
             [](const Json&) -> Result<Json> { return Json{{"state", "verified"}}; });
  }

  PollResult poll_at(std::int64_t second, bool dashboard_open,
                     const std::vector<Request>& requests = {}) {
    return poller_.poll(from_unix_seconds(second), dashboard_open, requests);
  }

  [[nodiscard]] FakeLink& link() { return link_; }
  [[nodiscard]] FakeRunner& runner() { return runner_; }
  void set_status(Json status) { status_ = std::move(status); }
  void offer(Json task) { task_ = std::move(task); }

 private:
  FakeLink link_;
  FakeRunner runner_;
  Poller poller_{link_, 600s, nullptr, &runner_};
  Json status_ = testing::cleaning_status_json(false);
  Json task_ = nullptr;
};

TEST_F(InboxPollerTest, PlansEveryTenSecondsAndTheBadgeCountsThePendingOnes) {
  DashboardModel model;
  PollResult first = poll_at(start, false);
  ASSERT_TRUE(first.cleanup.plans.has_value());
  EXPECT_FALSE(first.cleanup.quarantine) << "only while the dashboard is open";
  model.apply(std::move(first));
  EXPECT_EQ(model.cleanup().pending(), 2);
  EXPECT_TRUE(model.tray_status().badge) << "plans wait for the owner";
  EXPECT_EQ(model.tooltip().substr(0, model.tooltip().find('\n')), "MyClean - 2 approvals waiting");

  link().clear_calls();
  EXPECT_FALSE(poll_at(start + 9, false).cleanup.plans);
  EXPECT_TRUE(link().calls_of("cleaning.status").empty());
  EXPECT_TRUE(poll_at(start + 10, false).cleanup.plans);
}

// Approvals spec, "Approve some": the owner's selection goes to plans.approve as is, the plans
// are fetched again at once, and a user-scope plan is taken for the runner in the same poll.
TEST_F(InboxPollerTest, ApprovalSendsTheSelectionRefreshesAndTakesTheUserTask) {
  (void)poll_at(start, true);
  link().clear_calls();
  link().on("plans.approve",
            [](const Json&) -> Result<Json> { return Json{{"state", "approved"}}; });
  offer(testing::native_task_json());
  const PollResult result = poll_at(
      start + 1, true,
      {ApprovePlan{.id = 8, .selection = Selection{.all = false, .items = {81}, .except = {}}}});
  const std::vector<FakeLink::Call> approved = link().calls_of("plans.approve");
  ASSERT_EQ(approved.size(), 1U);
  EXPECT_EQ(approved[0].params,
            (Json{{"id", 8}, {"selection", {{"all", false}, {"items", {81}}}}}));
  EXPECT_EQ(result.cleanup.action, std::string());
  EXPECT_TRUE(result.cleanup.plans) << "fetched again after the decision";
  ASSERT_EQ(runner().started.size(), 1U);
  EXPECT_EQ(runner().started[0].at("plan"), 8);
  EXPECT_EQ(result.cleanup.task_activity, "running");

  // While it runs no other task is taken.
  link().clear_calls();
  (void)poll_at(start + 2, true);
  EXPECT_TRUE(link().calls_of("user_tasks.next").empty());
}

TEST_F(InboxPollerTest, RejectUndoRestoreScanAndProposeSendTheirParameters) {
  (void)poll_at(start, true);
  for (const char* method : {"plans.reject", "plans.undo", "quarantine.restore", "cleaning.scan",
                             "plans.propose_cleanup", "quarantine.purge"}) {
    link().on(method, [](const Json&) -> Result<Json> { return Json::object(); });
  }
  link().clear_calls();
  (void)poll_at(start + 1, true,
                {RejectPlan{.id = 7}, UndoPlan{.id = 5}, RestoreQuarantine{.plan = 3},
                 RestoreQuarantine{.plan = 3, .item = 31}, ScanNow{},
                 ProposeCleanup{.candidates = {701}}, PurgeQuarantine{.plan = 4}});
  EXPECT_EQ(link().calls_of("plans.reject")[0].params, (Json{{"id", 7}}));
  EXPECT_EQ(link().calls_of("plans.undo")[0].params, (Json{{"id", 5}}));
  const std::vector<FakeLink::Call> restores = link().calls_of("quarantine.restore");
  ASSERT_EQ(restores.size(), 2U);
  EXPECT_EQ(restores[0].params, (Json{{"plan", 3}}));
  EXPECT_EQ(restores[1].params, (Json{{"plan", 3}, {"item", 31}}));
  EXPECT_EQ(link().calls_of("cleaning.scan").size(), 1U);
  EXPECT_EQ(link().calls_of("plans.propose_cleanup")[0].params,
            (Json{{"candidates", {{"ids", {701}}}}}));
  EXPECT_EQ(link().calls_of("quarantine.purge")[0].params, (Json{{"plan", 4}}));
}

TEST_F(InboxPollerTest, ARefusedDecisionIsShownWithItsReason) {
  (void)poll_at(start, true);
  link().on("plans.approve", [](const Json&) -> Result<Json> {
    return fail(Errc::invalid_state, "json-rpc error -32000: the plan does not await approval");
  });
  const PollResult result =
      poll_at(start + 1, true, {ApprovePlan{.id = 7, .selection = select_all()}});
  ASSERT_TRUE(result.cleanup.action.has_value());
  EXPECT_EQ(*result.cleanup.action,
            "Approve failed: json-rpc error -32000: the plan does not await approval");
}

TEST_F(InboxPollerTest, OpenPlansAreFetchedAtOnceAndWithEveryRefreshUntilClosed) {
  (void)poll_at(start, true);
  link().clear_calls();
  const PollResult opened = poll_at(start + 1, true, {OpenPlan{.id = 7}});
  ASSERT_EQ(opened.cleanup.details.size(), 1U);
  EXPECT_EQ(opened.cleanup.details[0].plan.id, 7);
  link().clear_calls();
  const PollResult refreshed = poll_at(start + 11, true);
  EXPECT_TRUE(refreshed.cleanup.quarantine);
  EXPECT_EQ(refreshed.cleanup.details.size(), 1U);
  (void)poll_at(start + 12, true, {ClosePlan{.id = 7}});
  link().clear_calls();
  EXPECT_TRUE(poll_at(start + 22, true).cleanup.details.empty());
  EXPECT_TRUE(link().calls_of("plans.get").empty());
}

TEST_F(InboxPollerTest, AnEndedScanRefreshesThePlans) {
  set_status(testing::cleaning_status_json(true));
  (void)poll_at(start, false);
  link().clear_calls();
  EXPECT_FALSE(poll_at(start + 1, false).cleanup.plans);
  EXPECT_EQ(link().calls_of("cleaning.status").size(), 1U) << "followed while it scans";
  set_status(testing::cleaning_status_json(false, start + 2));
  EXPECT_TRUE(poll_at(start + 2, false).cleanup.plans);
}

TEST_F(InboxPollerTest, ASpaceListIsAskedAgainUntilItsAnalysisIsReady) {
  int calls = 0;
  link().on("space.top", [&calls](const Json&) -> Result<Json> {
    ++calls;
    return testing::space_top_json(calls < 2 ? "scanning" : "ready");
  });
  (void)poll_at(start, true);
  link().clear_calls();
  const PollResult first = poll_at(
      start + 1, true, {LoadSpace{.list = SpaceList::top, .volume = "C:", .refresh = true}});
  ASSERT_EQ(first.cleanup.space.size(), 1U);
  EXPECT_EQ(first.cleanup.space[0].state, "scanning");
  EXPECT_EQ(first.cleanup.space[0].volume, "C:");
  EXPECT_EQ(link().calls_of("space.top")[0].params, (Json{{"path", "C"}, {"refresh", true}}));
  link().clear_calls();
  const PollResult second = poll_at(start + 2, true);
  ASSERT_EQ(second.cleanup.space.size(), 1U);
  EXPECT_EQ(second.cleanup.space[0].state, "ready");
  EXPECT_EQ(link().calls_of("space.top")[0].params, (Json{{"path", "C"}}));
  link().clear_calls();
  EXPECT_TRUE(poll_at(start + 3, true).cleanup.space.empty());
  EXPECT_TRUE(link().calls_of("space.top").empty());
}

TEST_F(InboxPollerTest, ASpaceMethodThatFailsIsShownAsFailed) {
  (void)poll_at(start, true);
  const PollResult result =
      poll_at(start + 1, true, {LoadSpace{.list = SpaceList::duplicates, .volume = ""}});
  ASSERT_EQ(result.cleanup.space.size(), 1U);
  EXPECT_EQ(result.cleanup.space[0].state, "failed");
  EXPECT_NE(result.cleanup.space[0].error.find("method not found"), std::string::npos);
}

// Results must be reported: a finished task's report goes out before any new task, and is
// kept while the service cannot be reached.
TEST_F(InboxPollerTest, TheReportIsDeliveredBeforeTheNextTaskAndKeptWhileTheServiceIsAway) {
  offer(testing::native_task_json());
  (void)poll_at(start, false);
  ASSERT_EQ(runner().started.size(), 1U);
  runner().finish(8);

  link().set_online(false);
  (void)poll_at(start + 1, false);
  EXPECT_EQ(runner().deliveries, 0);
  EXPECT_TRUE(runner().finished.has_value());

  link().set_online(true);
  link().clear_calls();
  const PollResult back = poll_at(start + 2, false);
  const std::vector<FakeLink::Call> reports = link().calls_of("user_tasks.report");
  ASSERT_EQ(reports.size(), 1U);
  EXPECT_EQ(reports[0].params.at("id"), 8);
  EXPECT_EQ(reports[0].params.at("result").at("state"), "verified");
  EXPECT_EQ(runner().deliveries, 1);
  EXPECT_EQ(back.cleanup.task_note, "Plan 8: done");
  const std::vector<FakeLink::Call>& calls = link().calls();
  const auto position = [&](const char* method) {
    return std::ranges::find(calls, std::string(method), &FakeLink::Call::method) - calls.begin();
  };
  EXPECT_LT(position("user_tasks.report"), position("user_tasks.next"));
}

TEST_F(InboxPollerTest, AReportTheServiceRefusesIsDroppedWithItsReason) {
  link().on("user_tasks.report", [](const Json&) -> Result<Json> {
    return fail(Errc::invalid_argument, "json-rpc error -32602: unknown member 'state'");
  });
  offer(testing::native_task_json());
  (void)poll_at(start, false);
  runner().finish(8);
  const PollResult result = poll_at(start + 1, false);
  EXPECT_EQ(runner().deliveries, 1) << "sending it again would be refused again";
  ASSERT_TRUE(result.cleanup.task_note.has_value());
  EXPECT_NE((*result.cleanup.task_note).find("refused the report of plan 8"), std::string::npos);
}

TEST_F(InboxPollerTest, NoTaskIsTakenFromAnUnreachableService) {
  link().set_online(false);
  offer(testing::native_task_json());
  (void)poll_at(start, false);
  EXPECT_TRUE(runner().started.empty());
  EXPECT_TRUE(link().calls_of("user_tasks.next").empty());
}

}  // namespace
}  // namespace myclean::tray_model
