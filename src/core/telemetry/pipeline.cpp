#include "core/telemetry/pipeline.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"

namespace myclean::telemetry {

Pipeline::Pipeline(const os::IClock& clock, store::Store& store) : clock_(&clock), store_(&store) {}

void Pipeline::add(std::unique_ptr<ICollector> collector) {
  entries_.push_back(Entry{.collector = std::move(collector)});
}

std::vector<Error> Pipeline::poll() {
  std::vector<Error> errors;
  for (Entry& entry : entries_) {
    const MonoTime started = clock_->monotonic();
    if (entry.due && *entry.due > started) {
      continue;
    }
    const WallTime now = clock_->now();
    const MonoTime next = entry.due.value_or(started) + entry.collector->interval();
    entry.due = next > started ? next : started + entry.collector->interval();

    Result<std::vector<Measurement>> measurements = entry.collector->collect(now, started);
    if (!measurements) {
      errors.push_back(Error{
          .code = measurements.error().code,
          .message = std::string(entry.collector->name()) + ": " + measurements.error().message});
      continue;
    }
    for (const Measurement& measurement : *measurements) {
      store_->record(measurement.key, Sample{.time = now, .value = measurement.value});
    }
  }
  if (Result<void> ticked = store_->tick(); !ticked) {
    errors.push_back(
        Error{.code = ticked.error().code, .message = "store: " + ticked.error().message});
  }
  return errors;
}

std::optional<MonoTime> Pipeline::next_due() const {
  std::optional<MonoTime> next;
  for (const Entry& entry : entries_) {
    const MonoTime due = entry.due.value_or(MonoTime::min());
    if (!next || due < *next) {
      next = due;
    }
  }
  return next;
}

}  // namespace myclean::telemetry
