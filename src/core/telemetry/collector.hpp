#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean::telemetry {

// One value of one series, taken at the time the pipeline passed to collect().
struct Measurement {
  SeriesKey key;
  double value = 0.0;
};

[[nodiscard]] inline Measurement measure(std::string_view metric, std::string entity,
                                         double value) {
  return Measurement{.key = {.metric = std::string(metric), .entity = std::move(entity)},
                     .value = value};
}

// A source of measurements that the pipeline samples at a fixed interval.
class ICollector {
 public:
  virtual ~ICollector() = default;
  ICollector(const ICollector&) = delete;
  ICollector& operator=(const ICollector&) = delete;
  ICollector(ICollector&&) = delete;
  ICollector& operator=(ICollector&&) = delete;

  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual std::chrono::seconds interval() const = 0;
  // Takes one sample. `monotonic` is for rates: the time since the previous sample.
  [[nodiscard]] virtual Result<std::vector<Measurement>> collect(WallTime now,
                                                                 MonoTime monotonic) = 0;

 protected:
  ICollector() = default;
};

// The newest value a collector published, for readers on other threads. A reader only copies a
// pointer under the lock, so a slow reader never delays the collector.
template <typename T>
class Latest {
 public:
  void publish(T value) {
    auto next = std::make_shared<const T>(std::move(value));
    const std::lock_guard lock(mutex_);
    value_ = std::move(next);
  }
  // Null until the first publish.
  [[nodiscard]] std::shared_ptr<const T> get() const {
    const std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const T> value_;
};

}  // namespace myclean::telemetry
