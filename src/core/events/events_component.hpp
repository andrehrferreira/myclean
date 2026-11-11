#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#include "core/events/connections.hpp"
#include "core/events/event_router.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::events {

// Process and network events as a service component, shaped like svc::IComponent (name,
// migrations, start, stop) so the service wraps it in a few lines:
//  - start() subscribes an EventRecorder (proc_start, net_minute) to the router, starts the event
//    source, then seeds the router with the running processes, and starts a worker thread that
//    closes network minutes every second and snapshots the TCP/UDP tables every minute;
//  - other modules (governor, apps) subscribe their sinks to router();
//  - every tick gives the stored starts whose image was verified since (ISignerLookup::
//    take_resolved) their signer.
// When the event source cannot start (for example, not elevated), start() still succeeds: the
// connection snapshots keep working and unavailable() says why events are missing.
class EventsComponent {
 public:
  static constexpr std::chrono::seconds tick_interval{1};
  static constexpr std::chrono::seconds connection_interval{60};

  // The sources must outlive the component; `signers` may be null.
  EventsComponent(IEventSource& source, IConnectionTable& connections,
                  telemetry::IProcessTable& processes, ISignerLookup* signers = nullptr);
  EventsComponent(const EventsComponent&) = delete;
  EventsComponent& operator=(const EventsComponent&) = delete;
  EventsComponent(EventsComponent&&) = delete;
  EventsComponent& operator=(EventsComponent&&) = delete;
  ~EventsComponent();

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): the IComponent shape
  [[nodiscard]] std::string_view name() const { return "events"; }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): the IComponent shape
  [[nodiscard]] std::vector<store::MigrationSet> migrations() const { return {event_migrations()}; }
  // Fails only when the store refuses the retention rules or the component already runs. The
  // store and clock must stay valid until stop() returns.
  [[nodiscard]] Result<void> start(store::Store& store, const os::IClock& clock);
  // Stops the source and the worker, then queues every open network minute for the store's next
  // flush. Idempotent.
  void stop();

  [[nodiscard]] EventRouter& router() { return router_; }
  // What is not working, each prefixed: "ETW unavailable: ...", "process seed: ...",
  // "connections: ..." (the last snapshot failed). Empty when everything works. Thread-safe.
  [[nodiscard]] std::vector<Error> unavailable() const;
  // The newest TCP/UDP table; null before the first successful snapshot. Thread-safe.
  [[nodiscard]] std::shared_ptr<const ConnectionView> connections() const {
    return connections_.get();
  }

  // The worker's periodic work: closes network minutes, backfills the signers resolved since,
  // and snapshots the connection tables when connection_interval has passed. Only between
  // start() and stop(); public for tests.
  void tick();

 private:
  IEventSource* source_;
  IConnectionTable* connection_table_;
  telemetry::IProcessTable* processes_;
  ISignerLookup* signers_;
  EventRouter router_;

  const os::IClock* clock_ = nullptr;
  std::unique_ptr<EventRecorder> recorder_;
  bool source_running_ = false;
  telemetry::Latest<ConnectionView> connections_;

  mutable std::mutex status_mutex_;  // guards the members below up to tick_mutex_
  std::vector<Error> unavailable_;
  std::optional<Error> connection_error_;

  std::mutex tick_mutex_;  // guards next_snapshot_
  std::optional<MonoTime> next_snapshot_;

  std::mutex wake_mutex_;
  std::condition_variable_any wake_;
  std::jthread worker_;
};

}  // namespace myclean::events
