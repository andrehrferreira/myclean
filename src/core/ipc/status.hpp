#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"

namespace myclean::ipc {

// The `status` method: no parameters, answers a StatusReply.
inline constexpr std::string_view status_method = "status";

struct StoreStatus {
  std::string path;                  // the database file, UTF-8
  std::optional<std::string> error;  // the last failed periodic write, if the latest one failed
};

// Health of the running service, for `myclean status --json` and the tray.
struct StatusReply {
  bool healthy = false;                // false when any part reports an error
  std::optional<std::string> version;  // the deploy manifest's version; none for a dev build
  std::string build;                   // the version compiled in (project version)
  std::string mode;                    // "service" (under the SCM) or "console"
  std::int64_t pid = 0;
  std::int64_t uptime_seconds = 0;
  StoreStatus store;
  std::vector<std::string> components;  // running components, in start order
};

// {"health": "ok"|"degraded", "version": string|null, "build", "mode", "pid", "uptime_s",
//  "store": {"path", "error": string|null}, "components": [names]}
[[nodiscard]] nlohmann::json to_json(const StatusReply& status);
// Fails with invalid_data when a member is missing or has the wrong type.
[[nodiscard]] Result<StatusReply> parse_status(const nlohmann::json& value);

}  // namespace myclean::ipc
