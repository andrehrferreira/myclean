#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "core/ipc/status.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// A connection to the service pipe, for the CLI, the MCP server and the tray. One call at a
// time; not thread-safe.
class PipeClient {
 public:
  static constexpr std::chrono::milliseconds default_timeout{10'000};

  // Connects to `name` (normally default_pipe_name), waiting up to `timeout` while every
  // instance is busy. Each later call must be answered within `timeout`. Fails with not_found
  // when no server runs, access_denied when the pipe's DACL refuses this account.
  [[nodiscard]] static Result<PipeClient> connect(
      std::wstring_view name, std::chrono::milliseconds timeout = default_timeout);

  // Sends one request text and returns the response text, both unframed.
  [[nodiscard]] Result<std::string> exchange(std::string_view request);
  // Calls `method` with named `params`; the result member, or the JSON-RPC error as an Error
  // (see ipc::parse_response).
  [[nodiscard]] Result<nlohmann::json> call(
      std::string_view method, const nlohmann::json& params = nlohmann::json::object());
  // The `status` method.
  [[nodiscard]] Result<ipc::StatusReply> status();

 private:
  PipeClient(wil::unique_hfile pipe, std::chrono::milliseconds timeout);

  wil::unique_hfile pipe_;
  std::chrono::milliseconds timeout_;
  std::int64_t next_id_ = 1;
};

}  // namespace myclean::platform
