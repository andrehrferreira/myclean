#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "core/os/clock.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"

namespace myclean::tray {

// The model the poll thread writes and the UI thread reads, under its mutex.
struct SharedModel {
  std::mutex mutex;
  tray_model::DashboardModel model;
};

// Polls the service once per second on its own thread, so a slow or absent service never blocks
// the UI, sends the dashboard's requests and the freeze watchdog's incidents, and applies each
// result to the shared model.
class PollThread {
 public:
  static constexpr std::chrono::seconds interval{1};

  // Starts polling at once. `link`, `clock`, `shared`, `incidents` and `tasks` (when set) must
  // outlive the thread. `on_update` runs on the poll thread after each result is applied (the
  // tray posts itself a message).
  PollThread(tray_model::IServiceLink& link, const os::IClock& clock, SharedModel& shared,
             std::function<void()> on_update, tray_model::IncidentQueue* incidents = nullptr,
             tray_model::ITaskRunner* tasks = nullptr);
  PollThread(const PollThread&) = delete;
  PollThread& operator=(const PollThread&) = delete;
  PollThread(PollThread&&) = delete;
  PollThread& operator=(PollThread&&) = delete;
  // Stops and joins; waits for a call in progress (bounded by the link's timeout).
  ~PollThread();

  // Volumes and process trees are polled only while the dashboard is open. Opening it polls at
  // once, so it shows current data without waiting for the next second.
  void set_dashboard_open(bool open);
  // Sends `request` with a poll that starts at once.
  void request(tray_model::Request request);
  // Polls at once (a user task finished: its report goes out without waiting for the second).
  void wake();

 private:
  void run(const std::stop_token& stop);

  tray_model::Poller poller_;
  const os::IClock* clock_;
  SharedModel* shared_;
  std::function<void()> on_update_;
  std::mutex mutex_;
  std::condition_variable_any wake_cv_;
  bool wake_ = false;
  bool dashboard_open_ = false;
  std::vector<tray_model::Request> requests_;
  std::jthread thread_;  // last: starts after every member above is ready
};

}  // namespace myclean::tray
