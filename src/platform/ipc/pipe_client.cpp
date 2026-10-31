#include "platform/ipc/pipe_client.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <wil/resource.h>
#include <windows.h>

#include "core/ipc/frame.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"

namespace myclean::platform {

Result<PipeClient> PipeClient::connect(std::wstring_view name, std::chrono::milliseconds timeout) {
  using std::chrono::steady_clock;
  const std::wstring path(name);
  const steady_clock::time_point deadline = steady_clock::now() + timeout;
  for (;;) {
    // Identification level only: the server may check who the client is, never act as it.
    wil::unique_hfile pipe(::CreateFileW(
        path.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr));
    if (pipe) {
      DWORD mode = PIPE_READMODE_MESSAGE;
      if (::SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr) == FALSE) {
        return pipe_io::failure("SetNamedPipeHandleState", ::GetLastError());
      }
      return PipeClient(std::move(pipe), timeout);
    }
    const DWORD error = ::GetLastError();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady_clock::now());
    if (error != ERROR_PIPE_BUSY || remaining.count() <= 0) {
      return pipe_io::failure("connect to " + pipe_io::utf8(path), error);
    }
    // Returns early when an instance frees up; the next CreateFile decides.
    ::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(remaining.count()));
  }
}

PipeClient::PipeClient(wil::unique_hfile pipe, std::chrono::milliseconds timeout)
    : pipe_(std::move(pipe)), timeout_(timeout) {}

Result<std::string> PipeClient::exchange(std::string_view request) {
  const auto timeout_ms = static_cast<DWORD>(timeout_.count());
  const Result<std::string> frame = ipc::encode_frame(request);
  if (!frame) {
    return tl::unexpected(frame.error());
  }
  if (Result<void> written = pipe_io::write_message(pipe_.get(), *frame, nullptr, timeout_ms);
      !written) {
    return tl::unexpected(written.error());
  }
  const Result<std::string> response =
      pipe_io::read_message(pipe_.get(), nullptr, timeout_ms, ipc::max_frame_size);
  if (!response) {
    return tl::unexpected(response.error());
  }
  return ipc::decode_frame(*response);
}

Result<nlohmann::json> PipeClient::call(std::string_view method, const nlohmann::json& params) {
  const std::int64_t id = next_id_++;
  return exchange(ipc::make_request(id, method, params))
      .and_then([id](const std::string& response) { return ipc::parse_response(response, id); });
}

Result<ipc::StatusReply> PipeClient::status() {
  return call(ipc::status_method).and_then([](const nlohmann::json& reply) {
    return ipc::parse_status(reply);
  });
}

}  // namespace myclean::platform
