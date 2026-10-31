#pragma once

#include <array>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

#include "core/mcp/client.hpp"
#include "core/os/clock.hpp"

namespace myclean::mcp {

// The MCP protocol versions this server speaks, newest first.
inline constexpr std::array<std::string_view, 4> protocol_versions{"2025-11-25", "2025-06-18",
                                                                   "2025-03-26", "2024-11-05"};

// An MCP server over the stdio transport: JSON-RPC 2.0 messages, one per line. It answers
// initialize, ping, tools/list and tools/call, and offers only read tools, each mapped to a
// service read method through `call`:
//   health_snapshot   the prompt-1 snapshot (see health_snapshot())
//   metrics_latest    metrics.latest
//   metrics_history   metrics.query over the last `hours`
//   top_processes     processes.top
//   list_volumes      volumes.list
// No tool can change anything: there is no tool that closes, suspends or reprioritizes a
// process, and none that creates a plan. A tool whose service call fails returns a tool error
// (isError) naming what is unavailable; the server keeps running.
//
// Protocol errors are JSON-RPC errors: -32700 for text that is not JSON, -32600 for a message
// that is not a request object (batches included), -32601 for an unknown method, -32602 for an
// unknown tool or malformed tools/call parameters. Arguments that do not fit a tool's input
// schema produce a tool error, so the model can correct them.
class Server {
 public:
  // `call` and `clock` must outlive the server.
  Server(Caller call, const os::IClock& clock, std::string version);

  // The response to one message, or nothing for a notification or a response.
  [[nodiscard]] std::optional<std::string> handle(std::string_view message);

  // Answers every line of `in` on `out` until `in` ends; flushes after each response.
  void serve(std::istream& in, std::ostream& out);

  // The tools/list result.
  [[nodiscard]] static nlohmann::json tools();

 private:
  [[nodiscard]] nlohmann::json initialize(const nlohmann::json& params) const;
  // The tools/call result, or a JSON-RPC error message for malformed parameters.
  [[nodiscard]] tl::expected<nlohmann::json, std::string> call_tool(
      const nlohmann::json& params) const;

  Caller call_;
  const os::IClock* clock_;
  std::string version_;
};

}  // namespace myclean::mcp
