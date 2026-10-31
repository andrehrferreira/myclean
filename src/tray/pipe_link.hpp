#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/tray_model/poller.hpp"
#include "platform/ipc/pipe_client.hpp"

namespace myclean::tray {

// The service link over the pipe. Connects on the first call and again after the connection
// broke, so the tray follows the service through restarts. One thread at a time.
class PipeLink final : public tray_model::IServiceLink {
 public:
  // `timeout` bounds the connect wait while every pipe instance is busy and each answer.
  PipeLink(std::wstring pipe_name, std::chrono::milliseconds timeout)
      : name_(std::move(pipe_name)), timeout_(timeout) {}

  [[nodiscard]] Result<nlohmann::json> call(std::string_view method,
                                            const nlohmann::json& params) override;
  [[nodiscard]] bool reachable() const override { return reachable_; }

 private:
  std::wstring name_;
  std::chrono::milliseconds timeout_;
  std::optional<platform::PipeClient> client_;
  bool reachable_ = false;
  std::int64_t next_id_ = 1;
};

}  // namespace myclean::tray
