#pragma once

#include <string>
#include <string_view>

// The read methods of the service's telemetry component. Times are Unix seconds (UTC); byte
// values are bytes; metric names and units are those of core/telemetry/metrics.hpp.
namespace myclean::ipc {

// No parameters. {"time", "cpu": {"utility", "frequency", "dpc", "interrupt", "queue",
// "logical_processors"}, "memory": {"total", "available", "commit", "commit_limit", "standby",
// "modified", "compressed", "pool_paged", "pool_nonpaged", "hard_faults"}, "process": {"count",
// "threads", "handles"}, "disks": [{"disk", "read_bytes", "write_bytes", "iops", "read_latency",
// "write_latency", "queue", "idle"}], "gpu": {"adapters": [{"adapter", "entity", "utilization",
// "dedicated", "shared", "engines": {type: %}}]} or null when the GPU collector is unavailable,
// "volumes": [as volumes.list], "unavailable": [one text per unavailable collector]}. A value a
// collector has not measured yet is missing from its object.
inline constexpr std::string_view metrics_latest_method = "metrics.latest";

// Parameters: metric (token, required), entity (token, default "system"), from (integer,
// required), to (integer, default now + 1 s, so the newest sample is in), max_points (integer
// 1-10000, default 1000).
// {"metric", "entity", "resolution": "raw"|"minute"|"hour", "points": [{"time", "min", "max",
// "mean", "count"}]}: the finest resolution whose retention covers `from`; when there are more
// than max_points points, consecutive points are merged into max_points buckets. Fails with
// invalid_argument for an unknown metric or from >= to.
inline constexpr std::string_view metrics_query_method = "metrics.query";

// Parameters: sort ("cpu" or "memory", default "cpu"), limit (integer 1-100, default 10),
// qualifying (boolean, default false: only trees above qualifying_cpu_percent CPU or
// qualifying_memory_percent of physical memory as private working set).
// {"time", "logical_processors", "memory_total", "filter": {"cpu_percent", "memory_percent"} or
// null, "total_trees", "trees": [{"pid", "image", "processes", "cpu_percent", "memory_percent",
// "private_working_set", "private_bytes", "working_set", "io_bytes_per_second", "handles",
// "threads", "members": [{"pid", "parent_pid", "image", "cpu_percent", "private_working_set",
// "private_bytes"}], "trust"}]}. CPU percentages are of all logical processors. The System Idle
// Process (PID 0) is idle time: never listed, nor counted in total_trees. "trust" is null until
// the trust signal (task K1) is wired; it will then hold the description, category, signer,
// path, trust flag and reasons of the tree's root process.
inline constexpr std::string_view processes_top_method = "processes.top";
inline constexpr double qualifying_cpu_percent = 5.0;
inline constexpr double qualifying_memory_percent = 5.0;
inline constexpr int max_top_trees = 100;

// No parameters. {"time", "volumes": [{"volume", "entity", "mount_points", "size", "free",
// "free_percent", "disks", "system", "fill": {"state": "filling"|"not_filling"|
// "insufficient_data", "bytes_per_day", "days_until_full": number|null}}]}. "system" marks the
// volume Windows runs from.
inline constexpr std::string_view volumes_list_method = "volumes.list";

// The token form of a series entity, for the entity parameter of metrics.query: every run of
// characters a token does not allow becomes one '_', and a trailing '_' is dropped ("C:" -> "C",
// "0,17" -> "0_17", "luid_0x0_0xD1B5_phys_0/3D" -> "luid_0x0_0xD1B5_phys_0_3D").
[[nodiscard]] std::string entity_token(std::string_view entity);

}  // namespace myclean::ipc
