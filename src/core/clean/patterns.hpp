#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/clean/probes.hpp"

// The text side of rule targets: environment variables, wildcards and path containment.
namespace myclean::clean {

// `text` with every %NAME% replaced by that variable's value. nullopt when a named variable is
// not set: the target does not exist on this machine. A '%' without a closing one is literal.
[[nodiscard]] std::optional<std::wstring> expand_variables(std::wstring_view text,
                                                           const IEnvironment& environment);

// Every expansion of `text`: one per combination of the values of the variables it names
// (IEnvironment::values), in order; empty when a named variable has no value here.
[[nodiscard]] std::vector<std::wstring> expand_all(std::wstring_view text,
                                                   const IEnvironment& environment);

// True when `pattern` contains '*' or '?'.
[[nodiscard]] bool has_wildcard(std::wstring_view pattern);

// True when the whole of `name` matches `pattern`: '*' is any run of characters, '?' exactly
// one. ASCII letters compare case-insensitively, as Windows compares names.
[[nodiscard]] bool wildcard_match(std::wstring_view name, std::wstring_view pattern);

// True when `path` is `base` or lies inside it, ASCII case-insensitively. Both must be spelled
// the same way (absolute, '\' separators, no trailing separator except at a volume root).
[[nodiscard]] bool within(const std::filesystem::path& path, const std::filesystem::path& base);

}  // namespace myclean::clean
