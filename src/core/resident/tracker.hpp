#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "core/resident/sampler.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::resident {

// The network bytes per process of the stored minutes that start in [from, to)
// (events::network_by_process).
using TrafficSource =
    std::function<Result<std::vector<ProcessTraffic>>(WallTime from, WallTime to)>;

// The resident-process history over the telemetry component's process tables: every table goes
// to the UsageSampler, every minute's use (with the network bytes of the minutes stored by then)
// to the History, and each closed 5-minute bucket to the leak detector. Not thread-safe.
class Tracker {
 public:
  // A network minute is read this long after it ends, when the events component has stored it.
  static constexpr std::chrono::minutes network_delay{2};

  // `store` (opened with resident_migrations()) must outlive the tracker.
  explicit Tracker(store::Store& store) : history_(store) {}

  // Reads the stored buckets of the last fine_span before `now`.
  [[nodiscard]] Result<void> load(WallTime now) { return history_.load(now); }

  // Observes `view`. Once its minute is past the previous one, records the use of that minute;
  // when that closes a 5-minute bucket, returns the leak reading (leak_reading) of every program
  // in the history, which ends the alert of one that stopped growing or runs no more. Otherwise
  // no readings. A failure to read the network minutes is returned after the minute is recorded
  // without them; they are read again at the next minute.
  [[nodiscard]] Result<std::vector<alerts::Reading>> tick(const telemetry::ProcessView& view,
                                                          const TrafficSource& traffic);

  // Records the use observed since the previous minute now (at a stop).
  void flush();

  [[nodiscard]] const History& history() const { return history_; }

 private:
  UsageSampler sampler_;
  History history_;
  std::optional<WallTime> minute_;         // the minute being observed
  std::optional<WallTime> network_until_;  // network minutes before this are recorded
};

}  // namespace myclean::resident
