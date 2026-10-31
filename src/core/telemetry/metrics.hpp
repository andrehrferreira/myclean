#pragma once

#include <string_view>

// The series the collectors record: metric name, entity and unit. System-wide values use the
// entity "system".
namespace myclean::telemetry::metric {

inline constexpr std::string_view system = "system";

// CPU, every 1 s. Entity "system" or a logical processor "<group>,<number>" ("0,17").
inline constexpr std::string_view cpu_utility = "cpu.utility";      // %, 0-100 (Task Manager)
inline constexpr std::string_view cpu_frequency = "cpu.frequency";  // % of maximum frequency
// Entity "system" only.
inline constexpr std::string_view cpu_dpc = "cpu.dpc";              // % DPC time
inline constexpr std::string_view cpu_interrupt = "cpu.interrupt";  // % interrupt time
// Threads ready to run, waiting for a processor; recorded by the process collector.
inline constexpr std::string_view cpu_queue = "cpu.queue";

// Memory, every 1 s, entity "system". Bytes unless noted.
inline constexpr std::string_view memory_total = "memory.total";
inline constexpr std::string_view memory_available = "memory.available";
inline constexpr std::string_view memory_commit = "memory.commit";
inline constexpr std::string_view memory_commit_limit = "memory.commit_limit";
inline constexpr std::string_view memory_standby = "memory.standby";
inline constexpr std::string_view memory_modified = "memory.modified";
inline constexpr std::string_view memory_pool_paged = "memory.pool_paged";
inline constexpr std::string_view memory_pool_nonpaged = "memory.pool_nonpaged";
inline constexpr std::string_view memory_hard_faults = "memory.hard_faults";  // pages read/s
// The working set of the "Memory Compression" process, recorded by the process collector.
inline constexpr std::string_view memory_compressed = "memory.compressed";

// Physical disks, every 1 s. Entity: the disk number ("0", "7").
inline constexpr std::string_view disk_read_bytes = "disk.read_bytes";        // bytes/s
inline constexpr std::string_view disk_write_bytes = "disk.write_bytes";      // bytes/s
inline constexpr std::string_view disk_iops = "disk.iops";                    // transfers/s
inline constexpr std::string_view disk_read_latency = "disk.read_latency";    // ms per read
inline constexpr std::string_view disk_write_latency = "disk.write_latency";  // ms per write
inline constexpr std::string_view disk_queue = "disk.queue";  // requests outstanding
inline constexpr std::string_view disk_idle = "disk.idle";    // % idle time, 0-100

// Volumes, every 60 s, bytes. Entity: the first mount point without its trailing backslash
// ("C:", "D:\mnt\data"), see volume_entity().
inline constexpr std::string_view volume_size = "volume.size";
inline constexpr std::string_view volume_free = "volume.free";

// GPU, every 1 s. Entity: the adapter, "luid_0x<high>_0x<low>_phys_<n>"; gpu.engine adds
// "/<engine type>" ("luid_0x00000000_0x0000D1B5_phys_0/3D").
inline constexpr std::string_view gpu_utilization = "gpu.utilization";  // %, busiest engine
inline constexpr std::string_view gpu_engine = "gpu.engine";            // %, per engine type
inline constexpr std::string_view gpu_dedicated = "gpu.dedicated";      // bytes
inline constexpr std::string_view gpu_shared = "gpu.shared";            // bytes

// Process table totals, every 1 s, entity "system".
inline constexpr std::string_view process_count = "process.count";
inline constexpr std::string_view process_threads = "process.threads";
inline constexpr std::string_view process_handles = "process.handles";

}  // namespace myclean::telemetry::metric
