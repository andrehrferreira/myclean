#include "platform/ipc/pipe_server.hpp"

#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/ipc/frame.hpp"
#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {

namespace {

constexpr DWORD buffer_size = 64 * 1024;
constexpr DWORD retry_ms = 1000;

}  // namespace

Result<std::unique_ptr<PipeServer>> PipeServer::start(PipeServerOptions options) {
  if (Result<void> checked = check_sid(options.owner_sid); !checked) {
    return tl::unexpected(checked.error());
  }
  const Result<std::string> server_sid = current_user_sid();
  if (!server_sid) {
    return tl::unexpected(server_sid.error());
  }
  Result<wil::unique_hlocal_security_descriptor> descriptor =
      make_security_descriptor(pipe_sddl(options.owner_sid, *server_sid == options.owner_sid));
  if (!descriptor) {
    return tl::unexpected(descriptor.error());
  }
  Result<wil::unique_event_nothrow> stop = pipe_io::make_event();
  if (!stop) {
    return tl::unexpected(stop.error());
  }
  std::unique_ptr<PipeServer> server(
      new PipeServer(std::move(options), std::move(*descriptor), std::move(*stop)));
  Result<wil::unique_hfile> first = server->create_instance(true);
  if (!first) {
    return tl::unexpected(first.error());
  }
  server->acceptor_ = std::thread([self = server.get(), pipe = std::move(*first)]() mutable {
    self->accept_loop(std::move(pipe));
  });
  return server;
}

PipeServer::PipeServer(PipeServerOptions options, wil::unique_hlocal_security_descriptor descriptor,
                       wil::unique_event_nothrow stop)
    : options_(std::move(options)), descriptor_(std::move(descriptor)), stop_(std::move(stop)) {}

PipeServer::~PipeServer() {
  stop();
}

void PipeServer::stop() {
  if (!acceptor_.joinable()) {
    return;
  }
  stop_.SetEvent();
  acceptor_.join();
  for (Connection& connection : connections_) {
    connection.thread.join();
  }
  connections_.clear();
}

Result<wil::unique_hfile> PipeServer::create_instance(bool first) {
  SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                 .lpSecurityDescriptor = descriptor_.get(),
                                 .bInheritHandle = FALSE};
  const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                          (first ? static_cast<DWORD>(FILE_FLAG_FIRST_PIPE_INSTANCE) : 0U);
  wil::unique_hfile pipe(::CreateNamedPipeW(
      options_.name.c_str(), open_mode,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, buffer_size, buffer_size, 0, &attributes));
  if (!pipe) {
    const DWORD error = ::GetLastError();
    if (first && error == ERROR_ACCESS_DENIED) {
      return fail(Errc::in_use,
                  "another process already owns the pipe " + pipe_io::utf8(options_.name));
    }
    return pipe_io::failure("CreateNamedPipe " + pipe_io::utf8(options_.name), error);
  }
  return pipe;
}

void PipeServer::accept_loop(wil::unique_hfile listening) {
  for (;;) {
    if (!listening) {
      Result<wil::unique_hfile> created = create_instance(false);
      if (!created) {
        report(created.error());
        if (::WaitForSingleObject(stop_.get(), retry_ms) != WAIT_TIMEOUT) {
          return;
        }
        continue;
      }
      listening = std::move(*created);
    }
    const Result<void> connected = pipe_io::connect(listening.get(), stop_.get());
    if (stop_.is_signaled()) {
      return;
    }
    if (!connected) {
      // The client left before it was served; the instance is reused, so the name stays owned.
      ::DisconnectNamedPipe(listening.get());
      continue;
    }
    // The next instance exists before the connected one is handed over and may close.
    wil::unique_hfile client(listening.release());
    if (Result<wil::unique_hfile> next = create_instance(false); next) {
      listening = std::move(*next);
    } else {
      report(next.error());
    }
    reap();
    Connection& connection = connections_.emplace_back();
    connection.thread = std::thread([this, &connection, pipe = std::move(client)]() mutable {
      serve(std::move(pipe));
      connection.done = true;
    });
  }
}

void PipeServer::serve(wil::unique_hfile pipe) {
  for (;;) {
    // Ends on disconnect, stop, or a message larger than any valid frame.
    const Result<std::string> request =
        pipe_io::read_message(pipe.get(), stop_.get(), INFINITE, ipc::max_frame_size);
    if (!request) {
      break;
    }
    const std::string response = options_.handler(*request);
    if (!pipe_io::write_message(pipe.get(), response, stop_.get(), INFINITE)) {
      break;
    }
  }
  ::DisconnectNamedPipe(pipe.get());
}

void PipeServer::report(const Error& error) const {
  if (options_.on_error) {
    options_.on_error(error);
  }
}

void PipeServer::reap() {
  for (auto connection = connections_.begin(); connection != connections_.end();) {
    if (connection->done) {
      connection->thread.join();
      connection = connections_.erase(connection);
    } else {
      ++connection;
    }
  }
}

}  // namespace myclean::platform
