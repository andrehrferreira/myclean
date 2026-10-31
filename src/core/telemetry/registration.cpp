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

// Adds the opened collector to the pipeline and returns it, or records why it is unavailable.
template <typename T>
T* add_or_report(Pipeline& pipeline, Registration& registration, std::string_view name,
                 Result<std::unique_ptr<T>> opened) {
  if (!opened) {
    registration.unavailable.push_back(Error{
        .code = opened.error().code, .message = std::string(name) + ": " + opened.error().message});
    return nullptr;
  }
  T* collector = opened->get();
  pipeline.add(std::move(*opened));
  return collector;
}

}  // namespace

Registration register_collectors(Pipeline& pipeline, IProcessTable& processes,
                                 IPerfCounters& counters, const IMemoryStatus& memory,
                                 const IVolumeInfo& volumes) {
  Registration registration;
  auto process_collector = std::make_unique<ProcessCollector>(processes);
  registration.live.processes = process_collector.get();
  pipeline.add(std::move(process_collector));
  add_or_report(pipeline, registration, "cpu", make_cpu_collector(counters));
  add_or_report(pipeline, registration, "memory", make_memory_collector(memory, counters));
  add_or_report(pipeline, registration, "disks", make_disk_collector(counters));
  registration.live.gpu =
      add_or_report(pipeline, registration, "gpu", GpuCollector::open(counters));
  auto volume_collector = std::make_unique<VolumeCollector>(volumes);
  registration.live.volumes = volume_collector.get();
  pipeline.add(std::move(volume_collector));
  return registration;
}

}  // namespace myclean::telemetry
