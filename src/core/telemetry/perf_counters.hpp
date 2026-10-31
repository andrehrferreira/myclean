#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

namespace myclean::telemetry {

// The value of one counter instance. `instance` is empty for a counter without instances.
struct CounterValue {
  std::string instance;
  double value = 0.0;
};

// Performance counters read together, at the same instant (one PDH query).
class ICounterQuery {
 public:
  virtual ~ICounterQuery() = default;
  ICounterQuery(const ICounterQuery&) = delete;
  ICounterQuery& operator=(const ICounterQuery&) = delete;
  ICounterQuery(ICounterQuery&&) = delete;
  ICounterQuery& operator=(ICounterQuery&&) = delete;

  // Adds a counter by its English path, such as "\PhysicalDisk(*)\Disk Read Bytes/sec", and
  // returns its index for values(). A "*" instance matches every instance present at each
  // collect, so instances that appear later are picked up. Fails when the counter or its object
  // does not exist on this machine.
  [[nodiscard]] virtual Result<std::size_t> add(std::string_view path) = 0;
  // Takes one sample of every counter.
  [[nodiscard]] virtual Result<void> collect() = 0;
  // The values of counter `index` at the last collect. A rate counter has a value only from its
  // second collect on; until then, and for an instance without a valid value, it is left out.
  [[nodiscard]] virtual Result<std::vector<CounterValue>> values(std::size_t index) const = 0;

 protected:
  ICounterQuery() = default;
};

// Opens counter queries. Implemented by src/platform/perf.
class IPerfCounters {
 public:
  virtual ~IPerfCounters() = default;
  IPerfCounters(const IPerfCounters&) = delete;
  IPerfCounters& operator=(const IPerfCounters&) = delete;
  IPerfCounters(IPerfCounters&&) = delete;
  IPerfCounters& operator=(IPerfCounters&&) = delete;

  [[nodiscard]] virtual Result<std::unique_ptr<ICounterQuery>> open() = 0;

 protected:
  IPerfCounters() = default;
};

}  // namespace myclean::telemetry
