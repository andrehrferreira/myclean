#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/snapshot.hpp"

// The tray's reading of the telemetry methods (contracts in core/ipc/telemetry.hpp).
namespace myclean::tray_model {

// metrics.latest.
struct LatestReply {
  WallTime time;
  // Every value as a series of core/telemetry/metrics.hpp: cpu.*, memory.* and process.* of
  // "system", disk.* of each disk number, gpu.* of each adapter, gpu.engine of
  // "<adapter>/<engine type>". Volumes are in `volumes`.
  MetricsSnapshot metrics;
  std::vector<VolumeRow> volumes;
  unsigned logical_processors = 0;
  std::vector<std::string> unavailable;  // collectors this machine does not offer, with reasons
};

[[nodiscard]] Result<LatestReply> parse_metrics_latest(const nlohmann::json& reply);

// metrics.query of `metric` for `entity` (any series entity; sent in token form) over
// [from, to), in Unix seconds.
[[nodiscard]] nlohmann::json metrics_query_params(std::string_view metric, std::string_view entity,
                                                  std::int64_t from, std::int64_t to);
// The points' means as samples, under the series key the service answered with.
[[nodiscard]] Result<Series> parse_metrics_query(const nlohmann::json& reply);

enum class ProcessSort { cpu, memory };

[[nodiscard]] nlohmann::json processes_top_params(ProcessSort sort, int limit);
[[nodiscard]] Result<std::vector<ProcessTreeRow>> parse_processes_top(const nlohmann::json& reply);

// The cpu.* entity of logical processor `index` of all groups ("0,17"), assuming groups of 64
// processors, which Windows forms on machines with up to 64 logical processors per NUMA node.
[[nodiscard]] std::string processor_entity(unsigned index);

// Unix seconds.
[[nodiscard]] std::int64_t unix_seconds(WallTime time);
[[nodiscard]] WallTime from_unix_seconds(std::int64_t seconds);

}  // namespace myclean::tray_model
