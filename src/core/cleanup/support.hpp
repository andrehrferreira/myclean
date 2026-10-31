#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/units.hpp"

// Helpers shared by the cleanup actions.
namespace myclean::cleanup {

// `text` without leading and trailing spaces, tabs and line ends.
[[nodiscard]] std::string_view trim(std::string_view text);

// The lines of `text` (LF or CRLF), trimmed, without empty ones.
[[nodiscard]] std::vector<std::string_view> lines(std::string_view text);

// The words of `text`, split at runs of spaces.
[[nodiscard]] std::vector<std::string> words(std::string_view text);

[[nodiscard]] std::filesystem::path from_utf8(std::string_view text);

// A size as tools print it: a decimal number, optional spaces and a unit (B, byte(s), KB, MB,
// GB, TB, PB, any case), where one KB is `base` bytes. Nothing when `text` is not one.
[[nodiscard]] std::optional<Bytes> parse_size(std::string_view text, std::uint64_t base);

// The Verification of a bytes metric where less is better.
[[nodiscard]] Verification compare(std::string metric, Bytes before, Bytes after);

}  // namespace myclean::cleanup
