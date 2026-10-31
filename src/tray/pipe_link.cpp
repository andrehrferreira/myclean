#include "tray/pipe_link.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/pipe_client.hpp"

namespace myclean::tray {

Result<nlohmann::json> PipeLink::call(std::string_view method, const nlohmann::json& params) {
  if (!client_) {
    Result<platform::PipeClient> client = platform::PipeClient::connect(name_, timeout_);
    if (!client) {
      reachable_ = false;
      return tl::unexpected(client.error());
    }
    client_.emplace(std::move(*client));
  }
  const std::int64_t id = next_id_++;
  const Result<std::string> response = client_->exchange(ipc::make_request(id, method, params));
  if (!response) {
    // The pipe broke (the service stopped or restarted): connect again on the next call.
    client_.reset();
    reachable_ = false;
    return tl::unexpected(response.error());
  }
  reachable_ = true;
  return ipc::parse_response(*response, id);
}

}  // namespace myclean::tray
