#pragma once

#include <iosfwd>

#include "cli/command.hpp"
#include "core/mcp/client.hpp"

namespace myclean::cli {

// `myclean space top <drive>|downloads|profile [--limit <n>] | old-downloads | forgotten-files |
// duplicates [--mode exact|photos] | used-installers | owner-decisions | dev-drive | vhdx |
// indexed-folders [--refresh]` (`line.words` starting with "space"): the space analyses of
// core/ipc/plans.hpp as text, or the service's answer as JSON with --json. The service runs each
// analysis in the background; while one runs the previous result is shown and says so. They only
// read: the ids they list are what a proposal names, made in the tray or over MCP. Returns a cli
// exit code.
[[nodiscard]] int space_command(const CommandLine& line, const mcp::Caller& call, std::ostream& out,
                                std::ostream& err);

}  // namespace myclean::cli
