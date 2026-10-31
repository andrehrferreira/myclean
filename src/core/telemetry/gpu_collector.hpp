#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::telemetry {

struct GpuEngineUse {
  std::string type;  // "3D", "Video Decode", ...
  double percent = 0.0;
};

// GPU use of one process, over every adapter it uses.
struct GpuProcess {
  std::uint32_t pid = 0;
  std::vector<GpuEngineUse> engines;  // by type
  Bytes dedicated;
  Bytes shared;
};

struct GpuView {
  WallTime time;
  std::vector<GpuProcess> processes;  // by PID
};

// Samples the "GPU Engine", "GPU Adapter Memory" and "GPU Process Memory" counters every 1 s.
// An engine's utilization is the sum over the processes using it; an engine type's is its
// busiest engine's (as Task Manager shows it), capped at 100 %. Records gpu.* per adapter (see
// metrics.hpp); per-process values are available through latest(). A process that just started
// using an engine reads 0 % on its first sample (the counter needs two readings).
class GpuCollector final : public ICollector {
 public:
  static constexpr std::size_t counter_count = 5;
  using Indices = std::array<std::size_t, counter_count>;

  // Fails when the machine has no GPU counters. `counters` must outlive the collector.
  [[nodiscard]] static Result<std::unique_ptr<GpuCollector>> open(IPerfCounters& counters);

  GpuCollector(std::unique_ptr<ICounterQuery> query, Indices indices);

  [[nodiscard]] std::string_view name() const override { return "gpu"; }
  [[nodiscard]] std::chrono::seconds interval() const override { return std::chrono::seconds{1}; }
  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime now, MonoTime monotonic) override;

  // The newest per-process values; null before the first sample. Thread-safe.
  [[nodiscard]] std::shared_ptr<const GpuView> latest() const { return latest_.get(); }

 private:
  std::unique_ptr<ICounterQuery> query_;
  Indices indices_;
  Latest<GpuView> latest_;
};

}  // namespace myclean::telemetry
