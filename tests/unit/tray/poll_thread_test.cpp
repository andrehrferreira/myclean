#include "tray/poll_thread.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../tray_model/inbox_fixtures.hpp"
#include "../tray_model/tray_model_test_support.hpp"
#include "core/input/incident.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/user_tasks/task.hpp"
#include "fakes/fake_clock.hpp"
#include "tray/task_worker.hpp"

namespace myclean::tray {
namespace {

using namespace std::chrono_literals;
using tray_model::testing::FakeLink;
using tray_model::testing::Json;

constexpr std::int64_t start = 1'000'000;

// Counts the updates the poll thread reports and lets the test wait for the next one.
class Updates {
 public:
  void notify() {
    const std::scoped_lock lock(mutex_);
    ++count_;
    changed_.notify_all();
  }

  // Waits (bounded) until at least `count` updates happened.
  bool wait_for(int count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 10s, [&] { return count_ >= count; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int count_ = 0;
};

class PollThreadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.on("metrics.latest",
             [](const Json&) -> Result<Json> { return tray_model::testing::latest_json(start); });
    link_.on("processes.top",
             [](const Json&) -> Result<Json> { return tray_model::testing::top_json(); });
  }

  [[nodiscard]] FakeLink& link() { return link_; }
  [[nodiscard]] const fakes::FakeClock& clock() const { return clock_; }
  [[nodiscard]] SharedModel& shared() { return shared_; }
  [[nodiscard]] Updates& updates() { return updates_; }

 private:
  FakeLink link_;
  fakes::FakeClock clock_{tray_model::from_unix_seconds(start)};
  SharedModel shared_;
  Updates updates_;
};

TEST_F(PollThreadTest, PollsAtOnceAndAppliesTheResult) {
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
    ASSERT_TRUE(updates().wait_for(1));
    const std::scoped_lock lock(shared().mutex);
    EXPECT_TRUE(shared().model.service_reachable());
    EXPECT_EQ(shared().model.metrics_time(), tray_model::from_unix_seconds(start));
    EXPECT_FALSE(shared().model.top_by_cpu()) << "process trees only while the dashboard is open";
  }
  EXPECT_FALSE(link().calls().empty());
}

TEST_F(PollThreadTest, OpeningTheDashboardPollsTheProcessTreesAtOnce) {
  PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
  ASSERT_TRUE(updates().wait_for(1));
  thread.set_dashboard_open(true);
  // The next poll runs at once (without waiting for the second) and carries the trees.
  ASSERT_TRUE(updates().wait_for(2));
  const std::scoped_lock lock(shared().mutex);
  const auto& trees = shared().model.top_by_cpu();
  ASSERT_TRUE(trees.has_value());
  EXPECT_EQ((*trees).size(), 2U);
}

TEST_F(PollThreadTest, ARequestIsSentAtOnceAndItsAnswerApplied) {
  link().on("processes.explain",
            [](const Json&) -> Result<Json> { return tray_model::testing::explain_json(); });
  PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
  ASSERT_TRUE(updates().wait_for(1));
  thread.request(tray_model::ExplainProcess{.pid = 101});
  ASSERT_TRUE(updates().wait_for(2));
  const std::scoped_lock lock(shared().mutex);
  const auto& explanation = shared().model.explanation();
  ASSERT_TRUE(explanation.has_value());
  ASSERT_TRUE((*explanation).has_value());
  EXPECT_EQ((*explanation).value().pid, 101U);
  EXPECT_EQ(link().calls_of("processes.explain").size(), 1U);
}

TEST_F(PollThreadTest, ForwardsTheWatchdogsIncidents) {
  int recorded = 0;
  link().on("input.record_incident", [&recorded](const Json&) -> Result<Json> {
    ++recorded;
    return Json{{"id", recorded}};
  });
  tray_model::IncidentQueue incidents;
  incidents.record(input::Incident{
      .chord = input::Chord::copy, .time = tray_model::from_unix_seconds(start), .latency = 400ms});
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); }, &incidents);
    ASSERT_TRUE(updates().wait_for(1));
  }
  EXPECT_EQ(recorded, 1);
  EXPECT_EQ(incidents.counts().sent, 1U);
  const std::scoped_lock lock(shared().mutex);
  const auto& counts = shared().model.incident_counts();
  ASSERT_TRUE(counts.has_value());
  EXPECT_EQ((*counts).sent, 1U);
}

TEST_F(PollThreadTest, StopsPromptlyWhileTheServiceIsDown) {
  link().set_online(false);
  const auto begin = std::chrono::steady_clock::now();
  {
    const PollThread thread(link(), clock(), shared(), [this] { updates().notify(); });
    ASSERT_TRUE(updates().wait_for(1));
    const std::scoped_lock lock(shared().mutex);
    EXPECT_FALSE(shared().model.service_reachable());
  }
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 5s);
}

// Approvals spec and cleaning spec, "Native Prune Commands Under the User Token": the owner's
// approval goes to the service through the poll thread, the user-scope task it releases runs on
// the worker (neither the UI nor the poll thread), and its report goes back to the service.
TEST_F(PollThreadTest, AnApprovalReleasesAUserTaskThatRunsOnTheWorkerAndIsReported) {
  link().on("plans.list",
            [](const Json&) -> Result<Json> { return tray_model::testing::plans_json(); });
  bool approved = false;
  link().on("user_tasks.next", [&approved](const Json&) -> Result<Json> {
    return Json{{"task", std::exchange(approved, false) ? tray_model::testing::native_task_json()
                                                        : Json(nullptr)}};
  });
  link().on("plans.approve", [&approved](const Json&) -> Result<Json> {
    approved = true;
    return Json{{"state", "approved"}};
  });
  std::promise<Json> reported;
  link().on("user_tasks.report", [&reported](const Json& params) -> Result<Json> {
    reported.set_value(params);
    return Json{{"state", "verified"}};
  });
  std::thread::id poll_thread;
  std::thread::id ran_on;
  PollThread* poll = nullptr;
  TaskWorker worker(
      [&ran_on](const user_tasks::UserTask& task, const std::stop_token& /*stop*/) {
        ran_on = std::this_thread::get_id();
        return user_tasks::TaskReport{.plan = task.plan,
                                      .state = PlanState::verified,
                                      .error = {},
                                      .undo = {},
                                      .verification = std::nullopt,
                                      .skipped = {}};
      },
      [&poll] { poll->wake(); });
  {
    PollThread thread(
        link(), clock(), shared(),
        [this, &poll_thread] {
          poll_thread = std::this_thread::get_id();
          updates().notify();
        },
        nullptr, &worker);
    poll = &thread;
    ASSERT_TRUE(updates().wait_for(1));
    thread.request(tray_model::ApprovePlan{.id = 8, .selection = tray_model::select_all()});
    std::future<Json> report = reported.get_future();
    ASSERT_EQ(report.wait_for(10s), std::future_status::ready);
    const Json params = report.get();
    EXPECT_EQ(params.at("id"), 8);
    EXPECT_EQ(params.at("result").at("state"), "verified");
    worker.stop();
  }
  EXPECT_NE(ran_on, std::thread::id{});
  EXPECT_NE(ran_on, poll_thread);
  EXPECT_NE(ran_on, std::this_thread::get_id());
  EXPECT_EQ(link().calls_of("plans.approve").size(), 1U);
  const std::scoped_lock lock(shared().mutex);
  EXPECT_EQ(shared().model.cleanup().pending(), 2);
}

}  // namespace
}  // namespace myclean::tray
