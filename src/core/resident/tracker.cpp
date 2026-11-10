#include "core/resident/tracker.hpp"

#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include "core/alerts/engine.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/history.hpp"
#include "core/resident/leak.hpp"
#include "core/resident/sampler.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::resident {

Result<std::vector<alerts::Reading>> Tracker::tick(const telemetry::ProcessView& view,
                                                   const TrafficSource& traffic) {
  const WallTime minute = std::chrono::floor<std::chrono::minutes>(view.time);
  std::vector<alerts::Reading> readings;
  std::optional<Error> problem;
  if (minute_ && minute != *minute_) {
    // The tables observed so far belong to the previous minute.
    std::vector<MinuteUse> use = sampler_.take();
    const WallTime until = minute - network_delay;
    const WallTime from = network_until_.value_or(until - std::chrono::minutes{1});
    if (traffic && until > from) {
      Result<std::vector<ProcessTraffic>> transfers = traffic(from, until);
      if (transfers) {
        sampler_.attribute(*transfers, use);
        network_until_ = until;
      } else {
        problem = std::move(transfers.error());
      }
    }
    if (history_.add(*minute_, use)) {
      for (const auto& [key, trend] : history_.trends()) {
        readings.push_back(leak_reading(key, steady_growth(trend, view.time)));
      }
    }
  }
  sampler_.observe(view);
  minute_ = minute;
  if (problem) {
    return tl::unexpected(std::move(*problem));
  }
  return readings;
}

void Tracker::flush() {
  if (minute_) {
    static_cast<void>(history_.add(*minute_, sampler_.take()));
  }
  history_.flush();
}

}  // namespace myclean::resident
