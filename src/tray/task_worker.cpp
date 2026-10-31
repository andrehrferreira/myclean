#include "tray/task_worker.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/user_tasks/task.hpp"

namespace myclean::tray {

TaskWorker::TaskWorker(Execute execute, std::function<void()> on_finished)
    : execute_(std::move(execute)),
      on_finished_(std::move(on_finished)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

void TaskWorker::stop() {
  thread_.request_stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool TaskWorker::idle() const {
  const std::scoped_lock lock(mutex_);
  return !running_ && !report_;
}

void TaskWorker::start(nlohmann::json task) {
  const Result<std::int64_t> plan = user_tasks::task_plan(task);
  if (!plan) {
    spdlog::warn("user task ignored: {}", plan.error().message);
    return;
  }
  Result<user_tasks::UserTask> parsed = user_tasks::parse_task(task);
  if (!parsed) {
    spdlog::warn("user task of plan {} not run: {}", *plan, parsed.error().message);
    finish(user_tasks::TaskReport{.plan = *plan,
                                  .state = PlanState::failed,
                                  .error = parsed.error().message,
                                  .undo = {},
                                  .verification = std::nullopt,
                                  .skipped = {}},
           {});
    return;
  }
  spdlog::info("user task of plan {} started: {}", *plan, parsed->title);
  const std::scoped_lock lock(mutex_);
  activity_ = "Running plan " + std::to_string(*plan) + ": " + parsed->title;
  next_ = *std::move(parsed);
  running_ = true;
  wake_.notify_one();
}

std::optional<tray_model::ITaskRunner::Report> TaskWorker::report() const {
  const std::scoped_lock lock(mutex_);
  return report_;
}

void TaskWorker::delivered() {
  const std::scoped_lock lock(mutex_);
  report_.reset();
}

std::string TaskWorker::activity() const {
  const std::scoped_lock lock(mutex_);
  return activity_;
}

void TaskWorker::finish(const user_tasks::TaskReport& report, const std::string& title) {
  std::string line = user_tasks::summary(report, title);
  spdlog::info("user task: {}", line);
  {
    const std::scoped_lock lock(mutex_);
    report_ = Report{.plan = report.plan,
                     .result = user_tasks::report_record(report),
                     .summary = std::move(line)};
    running_ = false;
    activity_.clear();
  }
  on_finished_();
}

void TaskWorker::run(const std::stop_token& stop) {
  while (true) {
    std::optional<user_tasks::UserTask> task;
    {
      std::unique_lock lock(mutex_);
      if (!wake_.wait(lock, stop, [this] { return next_.has_value(); })) {
        return;
      }
      task = std::exchange(next_, std::nullopt);
    }
    if (task) {
      finish(execute_(*task, stop), task->title);
    }
  }
}

}  // namespace myclean::tray
