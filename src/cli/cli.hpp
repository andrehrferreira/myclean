#pragma once

#include <chrono>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include "core/mcp/client.hpp"

namespace myclean::cli {

// Exit codes of run().
inline constexpr int exit_ok = 0;
inline constexpr int exit_unavailable = 1;  // the service is unreachable, or a call failed
inline constexpr int exit_degraded = 2;     // `status`: the service answers but is degraded
inline constexpr int exit_usage = 3;

// How long a command waits to connect and then for each answer. `status` makes one call, so it
// answers within 2 x 8 s, inside the 20 s of deploy contract C6.
inline constexpr std::chrono::milliseconds default_timeout{8'000};

// myclean.exe: the read-only command line and MCP server (see usage()). `args` are the
// arguments after the program name, UTF-8. Results go to `out` (one JSON document with --json),
// diagnostics to `err`; `mcp` reads its messages from `in`.
[[nodiscard]] int run(std::span<const std::string> args, std::istream& in, std::ostream& out,
                      std::ostream& err, std::chrono::milliseconds timeout = default_timeout);

[[nodiscard]] std::string_view usage();

// A Caller that connects to `pipe` (\\.\pipe\<name>) for each call. Connection failures and
// timeouts are reported as not reached.
[[nodiscard]] mcp::Caller pipe_caller(std::wstring pipe, std::chrono::milliseconds timeout);

}  // namespace myclean::cli
