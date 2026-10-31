#include "core/events/event_router.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <utility>

#include "core/events/events.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::events {

EventRouter::EventRouter(ISignerLookup* signers) : signers_(signers) {}

void EventRouter::seed(std::span<const telemetry::ProcessInfo> processes) {
  const std::lock_guard lock(live_mutex_);
  for (const telemetry::ProcessInfo& info : processes) {
    live_.try_emplace(info.pid, std::make_shared<const ProcessStart>(ProcessStart{
                                    .process = {.pid = info.pid, .created = info.created},
                                    .parent_pid = info.parent_pid,
                                    .image = info.image}));
  }
}

void EventRouter::subscribe(IEventSink& sink) {
  const std::unique_lock lock(sinks_mutex_);
  sinks_.push_back(&sink);
}

void EventRouter::unsubscribe(const IEventSink& sink) {
  const std::unique_lock lock(sinks_mutex_);
  std::erase(sinks_, &sink);
}

std::shared_ptr<const ProcessStart> EventRouter::process(std::uint32_t pid) const {
  const std::lock_guard lock(live_mutex_);
  const auto found = live_.find(pid);
  return found == live_.end() ? nullptr : found->second;
}

std::optional<ProcessKey> EventRouter::parent_of(std::uint32_t pid, WallTime child_created) const {
  const std::lock_guard lock(live_mutex_);
  const auto found = live_.find(pid);
  if (found == live_.end() || found->second->process.created >= child_created) {
    return std::nullopt;
  }
  return found->second->process;
}

void EventRouter::on_start(const ProcessStart& event) {
  auto start = std::make_shared<ProcessStart>(event);
  start->parent = parent_of(event.parent_pid, event.process.created);
  if (signers_ != nullptr) {
    start->signer = signers_->signer(start->image);
  }
  {
    const std::lock_guard lock(live_mutex_);
    live_.insert_or_assign(event.process.pid, start);
  }
  const std::shared_lock lock(sinks_mutex_);
  for (IEventSink* sink : sinks_) {
    sink->on_start(*start);
  }
}

void EventRouter::on_stop(const ProcessStop& event) {
  {
    const std::lock_guard lock(live_mutex_);
    const auto found = live_.find(event.process.pid);
    if (found != live_.end() && found->second->process == event.process) {
      live_.erase(found);
    }
  }
  const std::shared_lock lock(sinks_mutex_);
  for (IEventSink* sink : sinks_) {
    sink->on_stop(event);
  }
}

void EventRouter::on_network(const NetworkTransfer& transfer) {
  NetworkTransfer routed = transfer;
  routed.process = process(transfer.pid);
  const std::shared_lock lock(sinks_mutex_);
  for (IEventSink* sink : sinks_) {
    sink->on_network(routed);
  }
}

}  // namespace myclean::events
