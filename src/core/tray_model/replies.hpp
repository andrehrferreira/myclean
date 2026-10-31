#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
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
  // The cpu.utility and cpu.frequency entities of the logical processors, in token form ("0_17"),
  // ordered by group then number.
  std::vector<std::string> processors;
  std::vector<std::string> unavailable;  // collectors this machine does not offer, with reasons
};

[[nodiscard]] Result<LatestReply> parse_metrics_latest(const nlohmann::json& reply);

// metrics.query of `metric` for `entity` (any series entity; sent in token form) over
// [from, to), in Unix seconds, merged into at most `max_points` points when set.
[[nodiscard]] nlohmann::json metrics_query_params(std::string_view metric, std::string_view entity,
                                                  std::int64_t from, std::int64_t to,
                                                  std::optional<int> max_points = std::nullopt);
// The points' means as samples, under the series key the service answered with.
[[nodiscard]] Result<Series> parse_metrics_query(const nlohmann::json& reply);

enum class ProcessSort { cpu, memory };

[[nodiscard]] nlohmann::json processes_top_params(ProcessSort sort, int limit);
[[nodiscard]] Result<std::vector<ProcessTreeRow>> parse_processes_top(const nlohmann::json& reply);

// processes.explain (core/ipc/telemetry.hpp).
[[nodiscard]] nlohmann::json processes_explain_params(std::uint32_t pid);
[[nodiscard]] Result<Explanation> parse_processes_explain(const nlohmann::json& reply);

// disks.health (core/ipc/disk_health.hpp): the drives in disk number order.
[[nodiscard]] Result<std::vector<DriveHealthRow>> parse_disks_health(const nlohmann::json& reply);

// The alert methods (core/ipc/alerts.hpp).
[[nodiscard]] Result<AlertSummary> parse_alerts_badge(const nlohmann::json& reply);
[[nodiscard]] nlohmann::json alerts_list_params(WallTime since);
[[nodiscard]] Result<std::vector<AlertRow>> parse_alerts_list(const nlohmann::json& reply);
[[nodiscard]] nlohmann::json alerts_snooze_params(std::string_view rule, std::string_view entity,
                                                  std::chrono::minutes duration);
[[nodiscard]] nlohmann::json alerts_dismiss_params(std::string_view rule, std::string_view entity);

// input.summary (core/ipc/input.hpp).
[[nodiscard]] Result<std::vector<FreezeGroupRow>> parse_input_summary(const nlohmann::json& reply);

// Unix seconds.
[[nodiscard]] std::int64_t unix_seconds(WallTime time);
[[nodiscard]] WallTime from_unix_seconds(std::int64_t seconds);

}  // namespace myclean::tray_model
