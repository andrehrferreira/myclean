#pragma once

#include <string>
#include <string_view>

// The read methods of the service's telemetry component. Times are Unix seconds (UTC); byte
// values are bytes; metric names and units are those of core/telemetry/metrics.hpp.
namespace myclean::ipc {

// No parameters. {"time", "cpu": {"utility", "frequency", "dpc", "interrupt", "queue",
// "logical_processors", "processors": [{"entity", "utility", "frequency"}]}, "memory": {"total",
// "available", "commit", "commit_limit", "standby", "modified", "compressed", "pool_paged",
// "pool_nonpaged", "hard_faults"}, "process": {"count", "threads", "handles"}, "disks": [{"disk",
// "read_bytes", "write_bytes", "iops", "read_latency", "write_latency", "queue", "idle"}], "gpu":
// {"adapters": [{"adapter", "entity", "utilization", "dedicated", "shared", "engines": {type: %}}]}
// or null when the GPU collector is unavailable, "volumes": [as volumes.list], "unavailable": [one
// text per unavailable collector]}. A value a collector has not measured yet is missing from its
// object. "processors" has one entry per logical processor, ordered by group then number; "entity"
// is the token form of its series entity ("0_17" for "0,17"), usable as the entity of
// metrics.query.
inline constexpr std::string_view metrics_latest_method = "metrics.latest";

// Parameters: metric (token, required), entity (token, default "system"), from (integer,
// required), to (integer, default now + 1 s, so the newest sample is in), max_points (integer
// 1-10000, default 1000).
// {"metric", "entity", "resolution": "raw"|"minute"|"hour", "points": [{"time", "min", "max",
// "mean", "count"}]}: the finest resolution whose retention covers `from`; when there are more
// than max_points points, consecutive points are merged into max_points buckets. A metric is
// known when a telemetry collector measures it now, or when the store holds a series of it (such
// as "disk.temperature" of disks.health, whose entity is the drive's "key"). Fails with
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
// Process (PID 0) is idle time: never listed, nor counted in total_trees. "trust" is the trust
// signal of the tree's root process (see below), or null when its image cannot be located or
// read (System, Registry, Memory Compression, a process that just exited).
inline constexpr std::string_view processes_top_method = "processes.top";
inline constexpr double qualifying_cpu_percent = 5.0;
inline constexpr double qualifying_memory_percent = 5.0;
inline constexpr int max_top_trees = 100;

// A trust signal (trust spec): facts only, never a verdict such as dangerous or malicious.
// {"status": "signed"|"unsigned"|"invalid", "signer": string|null (set only when signed),
//  "path": the image path, "path_class": "protected"|"user_writable", "unrecognized": bool,
//  "reasons": [why unrecognized: "not-in-knowledge-base", "unsigned", "invalid-signature",
//  "signer-not-listed", "user-writable-path"; empty unless unrecognized],
//  "description": string|null, "category": string|null (from the knowledge-base entry; null
//  when no entry matches)}
inline constexpr std::string_view trust_signed = "signed";
inline constexpr std::string_view trust_unsigned = "unsigned";
inline constexpr std::string_view trust_invalid = "invalid";
inline constexpr std::string_view path_protected = "protected";
inline constexpr std::string_view path_user_writable = "user_writable";

// Parameters: pid (integer, required). One running process explained:
// {"pid", "parent_pid", "image", "path": string|null, "cpu_percent", "memory_percent",
//  "private_working_set", "private_bytes", "working_set",
//  "tree": {"pid" (the root), "image", "processes", "cpu_percent", "memory_percent"} or null
//  when the process belongs to no tree of the sample,
//  "members": [the tree's processes, as the members of processes.top; empty when "tree" is
//  null],
//  "trust": as above, or null when the image cannot be located or read,
//  "kb": {"description", "category", "needed_at_startup", "safe_to_remove"} or null when no
//  knowledge-base entry matches}
// Percentages as in processes.top. Fails with not_found when no process has the PID in the
// newest sample ("process <pid> was not found"), invalid_argument for a negative PID.
inline constexpr std::string_view processes_explain_method = "processes.explain";

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
