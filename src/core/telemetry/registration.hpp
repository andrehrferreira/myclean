#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "core/model/result.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/gpu_collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/telemetry/process_table.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::telemetry {

// The collectors that publish a current view besides their series. Owned by the pipeline;
// valid while it lives. Null when that collector is unavailable.
struct LiveViews {
  const ProcessCollector* processes = nullptr;
  const GpuCollector* gpu = nullptr;
  const VolumeCollector* volumes = nullptr;
};

struct Registration {
  LiveViews live;
  // One error per collector that could not be opened on this machine, prefixed with its name
  // ("gpu: ..."); the other collectors are registered regardless.
  std::vector<Error> unavailable;
};

// What the pipeline gets instead of a collector (the service wraps each in a Tap that keeps its
// newest measurements); it must pass every call through to the collector it wraps.
using CollectorWrap = std::function<std::unique_ptr<ICollector>(std::unique_ptr<ICollector>)>;

// Adds MyClean's phase-1 collectors to `pipeline`: processes, cpu, memory, disks and gpu every
// 1 s, volumes every 60 s, each through `wrap` when it is set. The live views point at the
// collectors themselves. The sources must outlive the pipeline.
[[nodiscard]] Registration register_collectors(Pipeline& pipeline, IProcessTable& processes,
                                               IPerfCounters& counters, const IMemoryStatus& memory,
                                               const IVolumeInfo& volumes,
                                               const CollectorWrap& wrap = {});

}  // namespace myclean::telemetry
