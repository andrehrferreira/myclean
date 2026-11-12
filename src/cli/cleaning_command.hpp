#pragma once

#include <iosfwd>
#include <optional>

#include "cli/command.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {

// The read methods of the cleaning component (core/ipc/plans.hpp) as commands:
//   startup list | boot-times      startup.list, startup.boot_times
//   plans list [--state <state>]   plans.list
//   plans get <id>                 plans.get
//   quarantine                     quarantine.list
//   cleaning                       cleaning.status
//   system-space                   system_space.status
// as text, or the service's answer as JSON with --json. Nothing here proposes, approves or
// restores: those are the tray's (or a proposal over MCP). Returns a cli exit code, or nullopt
// when `line` is none of these commands.
[[nodiscard]] std::optional<int> cleaning_command(const CommandLine& line, const mcp::Caller& call,
                                                  std::ostream& out, std::ostream& err);

}  // namespace myclean::cli
