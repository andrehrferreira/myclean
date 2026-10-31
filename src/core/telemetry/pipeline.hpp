#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"

namespace myclean::telemetry {

// Samples collectors at their intervals and records what they measure in the store. It owns no
// thread: the service's telemetry thread calls poll(), then sleeps until next_due(). Not
// thread-safe; queries go to the store and to the collectors' latest() views, which are.
class Pipeline {
 public:
  // `clock` and `store` must outlive the pipeline.
  Pipeline(const os::IClock& clock, store::Store& store);

  // Adds a collector; it is due at the next poll.
  void add(std::unique_ptr<ICollector> collector);

  // Runs every due collector and records its measurements (Store::record) at the wall time it
  // started, then calls Store::tick(). Returns the failures, each prefixed with the collector's
  // name; a failing collector records nothing this time and is retried at its next interval.
  // A collector is next due one interval after its previous due time; intervals missed while
  // the machine slept or the thread stalled are skipped, not caught up.
  [[nodiscard]] std::vector<Error> poll();

  // When the next collector is due; nullopt when there is none.
  [[nodiscard]] std::optional<MonoTime> next_due() const;

 private:
  struct Entry {
    std::unique_ptr<ICollector> collector;
    std::optional<MonoTime> due;  // nullopt: at the next poll
  };

  const os::IClock* clock_;
  store::Store* store_;
  std::vector<Entry> entries_;
};

}  // namespace myclean::telemetry
