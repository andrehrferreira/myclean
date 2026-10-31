#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

namespace myclean::mcp {

// Why a call to the service failed.
struct CallError {
  // False when the service could not be asked at all: not running, the pipe refused this
  // account, or no answer in time. True when the service answered with an error.
  bool reached = false;
  std::string message;
};

using Reply = tl::expected<nlohmann::json, CallError>;

// Calls one read method of the service (core/ipc/telemetry.hpp) with named parameters.
using Caller = std::function<Reply(std::string_view method, const nlohmann::json& params)>;

// What the owner is told when the service cannot be asked: which features need it and why
// they are unavailable now.
[[nodiscard]] std::string unavailable_text(std::string_view reason);

// The prompt-1 snapshot, for `myclean health` and the health_snapshot tool: the metrics.latest
// document (cpu, memory, process, disks, gpu, volumes, unavailable), the members of a
// processes.top answer for the qualifying trees (time, logical_processors, memory_total, filter,
// total_trees, trees, each with its root's "trust"), "signals": {"commit_percent",
// "system_volume", "system_volume_free_percent", "disk_queue"}, each null when its source is
// missing, and "unrecognized": [{"pid", "image", "path", "reasons"}] for the trees whose root is
// flagged unrecognized. The per-processor list of metrics.latest is left out.
[[nodiscard]] Reply health_snapshot(const Caller& call);

}  // namespace myclean::mcp
