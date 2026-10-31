#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/tray_model/poller.hpp"
#include "core/user_tasks/task.hpp"

namespace myclean::tray {

// Runs the user tasks the poll thread takes from the service, one at a time, on its own thread
// (never the UI or the poll thread), and keeps each report until the poll thread delivered it.
// A task the tray cannot read is reported as failed without running anything.
class TaskWorker final : public tray_model::ITaskRunner {
 public:
  // Runs one task to its end; `stop` is requested when the tray quits.
  using Execute = std::function<user_tasks::TaskReport(const user_tasks::UserTask& task,
                                                       const std::stop_token& stop)>;

  // `on_finished` runs on the worker thread after each report is ready (the tray wakes the poll
  // thread, which delivers it).
  TaskWorker(Execute execute, std::function<void()> on_finished);
  TaskWorker(const TaskWorker&) = delete;
  TaskWorker& operator=(const TaskWorker&) = delete;
  TaskWorker(TaskWorker&&) = delete;
  TaskWorker& operator=(TaskWorker&&) = delete;
  ~TaskWorker() override { stop(); }

  // Cancels the task in progress and joins the worker thread; later tasks never run. A report
  // made then is not delivered: the service still sees the plan as handed out. on_finished does
  // not run after it returns, so the tray calls it before the poll thread goes.
  void stop();

  [[nodiscard]] bool idle() const override;
  void start(nlohmann::json task) override;
  [[nodiscard]] std::optional<Report> report() const override;
  void delivered() override;
  [[nodiscard]] std::string activity() const override;

 private:
  void run(const std::stop_token& stop);
  void finish(const user_tasks::TaskReport& report, const std::string& title);

  Execute execute_;
  std::function<void()> on_finished_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<user_tasks::UserTask> next_;
  bool running_ = false;
  std::string activity_;
  std::optional<Report> report_;
  std::jthread thread_;  // last: starts after every member above is ready
};

}  // namespace myclean::tray
