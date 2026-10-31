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

// Health of one service component (see svc::IComponent::health).
struct ComponentHealth {
  std::string name;
  bool healthy = true;
  std::vector<std::string> problems;     // why it is degraded; empty when healthy
  std::vector<std::string> unavailable;  // parts this machine does not offer ("gpu: ...")
};

// One knowledge pack folder (docs/packs.md) as the service last loaded it.
struct PackStatus {
  std::string id;
  std::string version;                 // empty when the pack was not loaded
  std::string origin;                  // builtin | owner
  std::string status;                  // loaded | disabled | shadowed | rejected
  bool applies = false;                // loaded, and its applies_when holds on this machine
  std::optional<std::string> message;  // why it was rejected
};

// Health of the running service, for `myclean status --json` and the tray.
struct StatusReply {
  bool healthy = false;                // false when the store or any component reports an error
  std::optional<std::string> version;  // the deploy manifest's version; none for a dev build
  std::string build;                   // the version compiled in (project version)
  std::string mode;                    // "service" (under the SCM) or "console"
  std::int64_t pid = 0;
  std::int64_t uptime_seconds = 0;
  StoreStatus store;
  std::vector<std::string> components;            // running components, in start order
  std::vector<ComponentHealth> component_health;  // the same components, in the same order
  std::vector<PackStatus> packs;                  // empty until the owner's session is known
};

// {"health": "ok"|"degraded", "version": string|null, "build", "mode", "pid", "uptime_s",
//  "store": {"path", "error": string|null}, "components": [names],
//  "component_health": [{"name", "health": "ok"|"degraded", "problems": [text],
//                        "unavailable": [text]}],
//  "packs": [{"id", "version", "origin", "status", "applies", "message": string|null}]}
// parse_status() accepts a reply without "component_health" or "packs" (a service older than
// the member).
[[nodiscard]] nlohmann::json to_json(const StatusReply& status);
// Fails with invalid_data when a member is missing or has the wrong type.
[[nodiscard]] Result<StatusReply> parse_status(const nlohmann::json& value);

}  // namespace myclean::ipc
