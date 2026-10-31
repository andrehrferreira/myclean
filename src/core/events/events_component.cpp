#include "core/events/events_component.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/events/connections.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::events {

namespace {

Error prefixed(std::string_view prefix, const Error& error) {
  return Error{.code = error.code, .message = std::string(prefix) + error.message};
}

}  // namespace

EventsComponent::EventsComponent(IEventSource& source, IConnectionTable& connections,
                                 telemetry::IProcessTable& processes, ISignerLookup* signers)
    : source_(&source), connection_table_(&connections), processes_(&processes), router_(signers) {}

EventsComponent::~EventsComponent() {
  stop();
}

Result<void> EventsComponent::start(store::Store& store, const os::IClock& clock) {
  if (recorder_) {
    return fail(Errc::invalid_state, "the events component is already running");
  }
  Result<void> retention = add_event_retention(store);
  if (!retention) {
    return retention;
  }
  clock_ = &clock;
  recorder_ = std::make_unique<EventRecorder>(store);
  router_.subscribe(*recorder_);

  std::vector<Error> unavailable;
  // The source starts before the snapshot, so a process that starts in between is not missed.
  Result<void> started = source_->start(router_);
  source_running_ = started.has_value();
  if (!started) {
    unavailable.push_back(prefixed("ETW unavailable: ", started.error()));
  }
  Result<std::vector<telemetry::ProcessInfo>> running = processes_->snapshot();
  if (running) {
    router_.seed(*running);
  } else {
    unavailable.push_back(prefixed("process seed: ", running.error()));
  }
  {
    const std::lock_guard lock(status_mutex_);
    unavailable_ = std::move(unavailable);
  }

  tick();
  worker_ = std::jthread([this](const std::stop_token& stop) {
    std::unique_lock lock(wake_mutex_);
    while (!wake_.wait_for(lock, stop, tick_interval, [] { return false; })) {
      if (stop.stop_requested()) {
        return;
      }
      lock.unlock();
      tick();
      lock.lock();
    }
  });
  return {};
}

void EventsComponent::stop() {
  if (!recorder_) {
    return;
  }
  if (source_running_) {
    source_->stop();
    source_running_ = false;
  }
  worker_.request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
  recorder_->close_all();
  router_.unsubscribe(*recorder_);
  recorder_.reset();
  clock_ = nullptr;
}

std::vector<Error> EventsComponent::unavailable() const {
  const std::lock_guard lock(status_mutex_);
  std::vector<Error> errors = unavailable_;
  if (connection_error_) {
    errors.push_back(*connection_error_);
  }
  return errors;
}

void EventsComponent::tick() {
  const std::lock_guard lock(tick_mutex_);
  recorder_->close_minutes(clock_->now());
  const MonoTime now = clock_->monotonic();
  if (next_snapshot_ && now < *next_snapshot_) {
    return;
  }
  next_snapshot_ = now + connection_interval;
  Result<std::vector<Connection>> snapshot = connection_table_->snapshot();
  if (snapshot) {
    connections_.publish({.time = clock_->now(), .connections = std::move(*snapshot)});
  }
  const std::lock_guard status(status_mutex_);
  connection_error_ =
      snapshot ? std::nullopt : std::optional<Error>{prefixed("connections: ", snapshot.error())};
}

}  // namespace myclean::events
