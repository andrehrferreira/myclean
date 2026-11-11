#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Plain-text renderings of the service's read results for people (the JSON shapes are those of
// core/ipc/status.hpp, telemetry.hpp, disk_health.hpp, alerts.hpp, input.hpp, governor.hpp and
// mcp::health_snapshot). A value the service did not report prints as "n/a".
namespace myclean::cli::text {

[[nodiscard]] std::string status(const nlohmann::json& status);
[[nodiscard]] std::string health(const nlohmann::json& snapshot);
[[nodiscard]] std::string system(const nlohmann::json& latest);        // metrics.latest
[[nodiscard]] std::string trees(const nlohmann::json& top);            // processes.top
[[nodiscard]] std::string volumes(const nlohmann::json& volumes);      // volumes.list "volumes"
[[nodiscard]] std::string history(const nlohmann::json& history);      // metrics.query
[[nodiscard]] std::string explain(const nlohmann::json& process);      // processes.explain
[[nodiscard]] std::string disks(const nlohmann::json& health);         // disks.health
[[nodiscard]] std::string alerts(const nlohmann::json& list);          // alerts.list
[[nodiscard]] std::string freezes(const nlohmann::json& summary);      // input.summary
[[nodiscard]] std::string governor(const nlohmann::json& status);      // governor.status
[[nodiscard]] std::string backups(const nlohmann::json& check);        // backups.check
[[nodiscard]] std::string installed_apps(const nlohmann::json& list);  // apps.list
[[nodiscard]] std::string map(const nlohmann::json& map);  // map::DiskMap (core/map/json)

}  // namespace myclean::cli::text
