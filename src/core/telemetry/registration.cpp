#include "core/telemetry/registration.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/gpu_collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/telemetry/process_table.hpp"
#include "core/telemetry/system_collectors.hpp"
#include "core/telemetry/volume_collector.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::telemetry {

namespace {

// Adds `collector` to the pipeline, through `wrap` when it is set, and returns it.
template <typename T>
T* add(Pipeline& pipeline, const CollectorWrap& wrap, std::unique_ptr<T> collector) {
  T* added = collector.get();
  if (wrap) {
    pipeline.add(wrap(std::move(collector)));
  } else {
    pipeline.add(std::move(collector));
  }
  return added;
}

// The same for a collector that may fail to open, or records why it is unavailable.
template <typename T>
T* add_or_report(Pipeline& pipeline, Registration& registration, const CollectorWrap& wrap,
                 std::string_view name, Result<std::unique_ptr<T>> opened) {
  if (!opened) {
    registration.unavailable.push_back(Error{
        .code = opened.error().code, .message = std::string(name) + ": " + opened.error().message});
    return nullptr;
  }
  return add(pipeline, wrap, *std::move(opened));
}

}  // namespace

Registration register_collectors(Pipeline& pipeline, IProcessTable& processes,
                                 IPerfCounters& counters, const IMemoryStatus& memory,
                                 const IVolumeInfo& volumes, const CollectorWrap& wrap) {
  Registration registration;
  registration.live.processes = add(pipeline, wrap, std::make_unique<ProcessCollector>(processes));
  add_or_report(pipeline, registration, wrap, "cpu", make_cpu_collector(counters));
  add_or_report(pipeline, registration, wrap, "memory", make_memory_collector(memory, counters));
  add_or_report(pipeline, registration, wrap, "disks", make_disk_collector(counters));
  registration.live.gpu =
      add_or_report(pipeline, registration, wrap, "gpu", GpuCollector::open(counters));
  registration.live.volumes = add(pipeline, wrap, std::make_unique<VolumeCollector>(volumes));
  return registration;
}

}  // namespace myclean::telemetry
