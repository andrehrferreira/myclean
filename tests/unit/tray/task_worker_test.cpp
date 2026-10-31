#include "tray/task_worker.hpp"

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
#include "core/model/plan.hpp"
#include "core/tray_model/poller.hpp"
#include "core/user_tasks/task.hpp"

namespace myclean::tray {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;
using Report = tray_model::ITaskRunner::Report;

// Counts finished tasks and lets the test wait for the next one.
class Finished {
 public:
  void notify() {
    const std::scoped_lock lock(mutex_);
    ++count_;
    changed_.notify_all();
  }
  bool wait_for(int count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 10s, [&] { return count_ >= count; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int count_ = 0;
};

user_tasks::TaskReport done(const user_tasks::UserTask& task) {
  return user_tasks::TaskReport{.plan = task.plan,
                                .state = PlanState::verified,
                                .error = {},
                                .undo = {},
                                .verification = std::nullopt,
                                .skipped = {}};
}

TEST(TaskWorkerTest, RunsATaskOnItsOwnThreadAndKeepsTheReportUntilDelivered) {
  Finished finished;
  std::thread::id ran_on;
  TaskWorker worker(
      [&](const user_tasks::UserTask& task, const std::stop_token& /*stop*/) {
        ran_on = std::this_thread::get_id();
        return done(task);
      },
      [&] { finished.notify(); });
  EXPECT_TRUE(worker.idle());
  worker.start(tray_model::testing::native_task_json());
  ASSERT_TRUE(finished.wait_for(1));
  EXPECT_NE(ran_on, std::this_thread::get_id()) << "never the caller's (poll) thread";
  EXPECT_FALSE(worker.idle()) << "the report waits for delivery";
  EXPECT_TRUE(worker.activity().empty());
  ASSERT_TRUE(worker.report().has_value());
  const Report report = worker.report().value_or(Report{});
  EXPECT_EQ(report.plan, 8);
  EXPECT_EQ(report.result, (Json{{"state", "verified"}, {"error", nullptr}}));
  EXPECT_EQ(report.summary, "Plan 8 (Run `pnpm store prune`): verified");
  worker.delivered();
  EXPECT_TRUE(worker.idle());
  EXPECT_FALSE(worker.report());
}

TEST(TaskWorkerTest, AnUnreadableTaskIsReportedAsFailedWithoutRunning) {
  Finished finished;
  bool ran = false;
  TaskWorker worker(
      [&](const user_tasks::UserTask& task, const std::stop_token& /*stop*/) {
        ran = true;
        return done(task);
      },
      [&] { finished.notify(); });
  Json recycle = tray_model::testing::native_task_json();
  recycle["kind"] = "cleanup";
  recycle["action"] = {{"recycle", true}};
  worker.start(recycle);
  ASSERT_TRUE(finished.wait_for(1));
  EXPECT_FALSE(ran);
  ASSERT_TRUE(worker.report().has_value());
  const Report report = worker.report().value_or(Report{});
  EXPECT_EQ(report.plan, 8);
  EXPECT_EQ(report.result.at("state"), "failed");
  EXPECT_NE(report.result.at("error").get<std::string>().find("cleanup"), std::string::npos);

  worker.delivered();
  worker.start(Json{{"title", "no plan id"}});
  EXPECT_TRUE(worker.idle()) << "nothing to report it under";
}

TEST(TaskWorkerTest, StopCancelsTheTaskInProgress) {
  std::promise<void> started;
  bool cancelled = false;
  TaskWorker worker(
      [&](const user_tasks::UserTask& task, const std::stop_token& stop) {
        started.set_value();
        std::mutex mutex;
        std::condition_variable_any wait;
        std::unique_lock lock(mutex);
        cancelled = !wait.wait_for(lock, stop, 10s, [] { return false; }) && stop.stop_requested();
        return done(task);
      },
      [] {});
  worker.start(tray_model::testing::native_task_json());
  ASSERT_EQ(started.get_future().wait_for(10s), std::future_status::ready);
  EXPECT_EQ(worker.activity(), "Running plan 8: Run `pnpm store prune`");
  const auto begin = std::chrono::steady_clock::now();
  worker.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 5s);
  EXPECT_TRUE(cancelled);
}

}  // namespace
}  // namespace myclean::tray
