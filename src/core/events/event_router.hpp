#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/events/events.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::events {

// Receives the raw events of an IEventSource, completes them and passes them to every
// subscribed sink:
//  - a start gets its parent instance (only a live process created before it, so a reused PID is
//    never taken for the parent) and its signer;
//  - a network transfer gets the live process that owns its PID.
// It keeps the live processes by PID: added on start, removed on stop.
//
// Thread-safe. Sinks are called on the event source's thread; a sink may call process() but
// must not subscribe or unsubscribe from inside a callback.
class EventRouter final : public IEventSink {
 public:
  // `signers` may be null (no signer lookup); it must outlive the router.
  explicit EventRouter(ISignerLookup* signers = nullptr);

  // Adds processes that were running before the event source started, so that their children
  // find their parent and their transfers find their process. A PID already known from an event
  // is kept. Seeded processes are not passed to sinks as starts.
  void seed(std::span<const telemetry::ProcessInfo> processes);

  // `sink` must stay valid until it is unsubscribed.
  void subscribe(IEventSink& sink);
  void unsubscribe(const IEventSink& sink);

  // The live process with this PID; null when it is not known.
  [[nodiscard]] std::shared_ptr<const ProcessStart> process(std::uint32_t pid) const;

  void on_start(const ProcessStart& event) override;
  void on_stop(const ProcessStop& event) override;
  void on_network(const NetworkTransfer& transfer) override;

 private:
  // The live process `pid` when it was created before `child_created`.
  [[nodiscard]] std::optional<ProcessKey> parent_of(std::uint32_t pid,
                                                    WallTime child_created) const;

  ISignerLookup* signers_;

  mutable std::mutex live_mutex_;  // guards live_
  std::unordered_map<std::uint32_t, std::shared_ptr<const ProcessStart>> live_;

  mutable std::shared_mutex sinks_mutex_;  // guards sinks_; held shared while dispatching
  std::vector<IEventSink*> sinks_;
};

}  // namespace myclean::events
