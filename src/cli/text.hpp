#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Plain-text renderings of the service's read results for people (the JSON shapes are those of
// core/ipc/status.hpp, core/ipc/telemetry.hpp and mcp::health_snapshot). A value the service did
// not report prints as "n/a".
namespace myclean::cli::text {

[[nodiscard]] std::string status(const nlohmann::json& status);
[[nodiscard]] std::string health(const nlohmann::json& snapshot);
[[nodiscard]] std::string system(const nlohmann::json& latest);    // metrics.latest
[[nodiscard]] std::string trees(const nlohmann::json& top);        // processes.top
[[nodiscard]] std::string volumes(const nlohmann::json& volumes);  // volumes.list "volumes"
[[nodiscard]] std::string history(const nlohmann::json& history);  // metrics.query

}  // namespace myclean::cli::text
