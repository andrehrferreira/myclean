#pragma once

#include <iosfwd>
#include <span>
#include <string>

#include "core/mcp/client.hpp"

namespace myclean::cli {

// `myclean report get <YYYY-MM> | compare <a> <b>` (`words` without "report"): the stored monthly
// report of core/ipc/reports.hpp (its Markdown and file paths) or the changes from snapshot <a>
// to snapshot <b>, as text, or the answer's JSON with `json`. A month without a report or
// snapshot is a successful read that says so ("found": false). Returns a cli exit code.
[[nodiscard]] int report_command(std::span<const std::string> words, bool json,
                                 const mcp::Caller& call, std::ostream& out, std::ostream& err);

}  // namespace myclean::cli
