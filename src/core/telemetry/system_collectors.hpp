#pragma once

#include <memory>

#include "core/model/result.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::telemetry {

// Collectors of CPU, memory and physical-disk series (see metrics.hpp), each sampled every 1 s
// through one counter query. Rate counters have values from the second sample on. Each fails to
// open when one of its counters does not exist on this machine. `counters` and `memory` must
// outlive the collectors.

// Utilization and % of maximum frequency, total and per logical processor; DPC and interrupt
// time. (The processor queue length comes from the process collector.)
[[nodiscard]] Result<std::unique_ptr<ICollector>> make_cpu_collector(IPerfCounters& counters);

// Every physical disk the counters report, discovered at each sample (USB disks included):
// bytes/s read and written, IOPS, latency per read and per write, queue length, % idle.
[[nodiscard]] Result<std::unique_ptr<ICollector>> make_disk_collector(IPerfCounters& counters);

// Total, available, commit charge and limit and pools from one IMemoryStatus reading; standby
// and modified lists and hard faults/s from counters.
[[nodiscard]] Result<std::unique_ptr<ICollector>> make_memory_collector(const IMemoryStatus& memory,
                                                                        IPerfCounters& counters);

}  // namespace myclean::telemetry
