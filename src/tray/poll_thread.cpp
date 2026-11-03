#include "tray/poll_thread.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

#include "core/os/clock.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/poller.hpp"

namespace myclean::tray {

PollThread::PollThread(tray_model::IServiceLink& link, const os::IClock& clock, SharedModel& shared,
                       std::function<void()> on_update, tray_model::IncidentQueue* incidents,
                       tray_model::ITaskRunner* tasks, tray_model::UserStateSource user_state)
    : poller_(link, tray_model::live_window, incidents, tasks, std::move(user_state)),
      clock_(&clock),
      shared_(&shared),
      on_update_(std::move(on_update)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

PollThread::~PollThread() {
  thread_.request_stop();
  thread_.join();
}

void PollThread::set_dashboard_open(bool open) {
  const std::scoped_lock lock(mutex_);
  if (open && !dashboard_open_) {
    wake_ = true;
    wake_cv_.notify_one();
  }
  dashboard_open_ = open;
}

void PollThread::request(tray_model::Request request) {
  const std::scoped_lock lock(mutex_);
  requests_.push_back(std::move(request));
  wake_ = true;
  wake_cv_.notify_one();
}

void PollThread::wake() {
  const std::scoped_lock lock(mutex_);
  wake_ = true;
  wake_cv_.notify_one();
}

void PollThread::run(const std::stop_token& stop) {
  auto next = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    bool dashboard_open = false;
    std::vector<tray_model::Request> requests;
    {
      const std::scoped_lock lock(mutex_);
      dashboard_open = dashboard_open_;
      requests.swap(requests_);
      wake_ = false;
    }
    tray_model::PollResult result = poller_.poll(clock_->now(), dashboard_open, requests);
    {
      const std::scoped_lock lock(shared_->mutex);
      shared_->model.apply(std::move(result));
    }
    on_update_();
    // Keep a steady 1 s cadence; after a long stall start counting again from now.
    next = std::max(next + interval, std::chrono::steady_clock::now());
    std::unique_lock lock(mutex_);
    wake_cv_.wait_until(lock, stop, next, [this] { return wake_; });
  }
}

}  // namespace myclean::tray
