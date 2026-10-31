#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {

// The service pipe. Tests use their own names, never this one.
inline constexpr std::wstring_view default_pipe_name = LR"(\\.\pipe\myclean)";

struct PipeServerOptions {
  std::wstring name;      // \\.\pipe\<name>
  std::string owner_sid;  // the only account besides SYSTEM that may open the pipe
  // Answers one request message with one response message; called concurrently from the
  // connection threads.
  std::function<std::string(std::string_view request)> handler;
  // Reports a failure to create the next pipe instance after start; the server retries every
  // second. May be empty.
  std::function<void(const Error&)> on_error;
};

// A message-mode named pipe server: one thread waits for clients, one thread per connected
// client reads a request message, answers it and waits for the next.
//
// The pipe refuses remote clients and carries an explicit protected DACL (see pipe_sddl): SYSTEM
// and the owner only. The first instance is created with FILE_FLAG_FIRST_PIPE_INSTANCE, so start
// fails when another process already owns the name; later instances are created while one
// still exists, so the name is never free for another process to take over.
class PipeServer {
 public:
  // Creates the first instance synchronously and starts accepting clients. Fails with in_use
  // when the pipe exists, invalid_argument when owner_sid is not a SID.
  [[nodiscard]] static Result<std::unique_ptr<PipeServer>> start(PipeServerOptions options);

  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;
  PipeServer(PipeServer&&) = delete;
  PipeServer& operator=(PipeServer&&) = delete;
  ~PipeServer();

  // Cancels every wait, disconnects every client and joins all threads. Idempotent; call from
  // one thread.
  void stop();

 private:
  struct Connection {
    std::thread thread;
    std::atomic<bool> done{false};
  };

  PipeServer(PipeServerOptions options, wil::unique_hlocal_security_descriptor descriptor,
             wil::unique_event_nothrow stop);

  [[nodiscard]] Result<wil::unique_hfile> create_instance(bool first);
  void accept_loop(wil::unique_hfile listening);
  void serve(wil::unique_hfile pipe);
  void report(const Error& error) const;
  // Joins the connection threads that have finished. Accept thread only.
  void reap();

  PipeServerOptions options_;
  wil::unique_hlocal_security_descriptor descriptor_;
  wil::unique_event_nothrow stop_;
  std::list<Connection> connections_;  // accept thread only, until stop() joined it
  std::thread acceptor_;
};

}  // namespace myclean::platform
