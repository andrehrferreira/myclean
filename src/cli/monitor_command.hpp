#pragma once

#include <iosfwd>
#include <optional>

#include "cli/command.hpp"
#include "core/mcp/client.hpp"
#include "core/os/clock.hpp"

namespace myclean::cli {

// The read methods of the watchers as commands:
//   governor events [--hours <n>] [--limit <n>]   governor.events (core/ipc/governor.hpp)
//   freezes pauses                                input.pauses (core/ipc/input.hpp)
//   alerts badge                                  alerts.badge (core/ipc/alerts.hpp)
//   resident costs | trend <key>                  resident.costs, resident.trend (resident.hpp)
// as text, or the service's answer as JSON with --json. --hours is measured back from `clock`.
// Nothing here acts on a process or an alert. Returns a cli exit code, or nullopt when `line` is
// none of these commands.
[[nodiscard]] std::optional<int> monitor_command(const CommandLine& line, const mcp::Caller& call,
                                                 const os::IClock& clock, std::ostream& out,
                                                 std::ostream& err);

}  // namespace myclean::cli
