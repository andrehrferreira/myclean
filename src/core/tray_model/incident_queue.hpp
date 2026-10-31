#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "core/input/incident.hpp"
#include "core/input/probe.hpp"
#include "core/tray_model/service_link.hpp"

namespace myclean::tray_model {

// Carries the freeze watchdog's incidents to the service (input.record_incident). The watchdog's
// probing thread only appends under a short lock, so it never waits for the pipe; the poll
// thread forwards. While the service is down the queue keeps the newest `capacity` incidents and
// counts the older ones it drops. Thread-safe.
class IncidentQueue final : public input::IIncidentSink {
 public:
  static constexpr std::size_t default_capacity = 256;

  struct Counts {
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;   // pushed out by newer incidents while the service was down
    std::uint64_t rejected = 0;  // answered with an error by the service: never resent
    std::size_t queued = 0;

    friend bool operator==(const Counts&, const Counts&) = default;
  };

  explicit IncidentQueue(std::size_t capacity = default_capacity) : capacity_(capacity) {}

  void record(const input::Incident& incident) override;

  // Sends the queued incidents, oldest first. Stops at the first call that does not reach the
  // service and keeps that incident and the later ones for the next call. Returns how many the
  // service stored.
  int forward(IServiceLink& link);

  [[nodiscard]] Counts counts() const;

 private:
  // Drops the oldest incidents beyond the capacity. Needs mutex_.
  void trim();

  std::size_t capacity_;
  mutable std::mutex mutex_;  // guards the members below
  std::deque<input::Incident> queue_;
  Counts counts_;
};

}  // namespace myclean::tray_model
