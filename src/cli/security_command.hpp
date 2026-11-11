#pragma once

#include <iosfwd>
#include <span>
#include <string>

#include "core/mcp/client.hpp"

namespace myclean::cli {

// `myclean security scan | extensions | network [--hours <n>] | defender-scan <id>` (`words`
// without "security"): the intruder scan of core/ipc/security.hpp as text, or its JSON with
// `json`. defender-scan starts a Microsoft Defender scan of one scan item's file that reports and
// removes nothing. Returns a cli exit code.
[[nodiscard]] int security_command(std::span<const std::string> words, const std::string* hours,
                                   bool json, const mcp::Caller& call, std::ostream& out,
                                   std::ostream& err);

}  // namespace myclean::cli
