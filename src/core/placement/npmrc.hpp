#pragma once

#include <optional>
#include <string>
#include <string_view>

// The `key=value` lines of an npm configuration file (.npmrc), read and edited the way
// `npm config get|set|delete` treat them: comments (";", "#") and every other line are kept as
// they are, and the last assignment of a key wins.
namespace myclean::placement {

// The value of `key` (surrounding quotes removed); nullopt when no line assigns it.
[[nodiscard]] std::optional<std::string> npmrc_value(std::string_view text, std::string_view key);

// `text` with `key` set to `value`: the last line assigning it is replaced, or a line is
// appended in the file's line-ending style.
[[nodiscard]] std::string npmrc_set(std::string_view text, std::string_view key,
                                    std::string_view value);

// `text` without any line assigning `key`.
[[nodiscard]] std::string npmrc_remove(std::string_view text, std::string_view key);

}  // namespace myclean::placement
