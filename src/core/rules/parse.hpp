#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/schema.hpp"

// Parsers for the data files described in schema.hpp. Each validates the whole file and returns
// either every entry or one Errc::invalid_data error, never a part of the file. Unknown keys,
// duplicate ids, wrong types and out-of-range values are errors. The message names the file,
// the entry (by position and id) and the field, e.g.
//   C:\MyClean\data\rules\dev.toml: rule #11 'dev.cargo.target': field 'tier' is missing
// `file` is used only in messages.
namespace myclean::rules {

[[nodiscard]] Result<std::vector<CleaningRule>> parse_rule_file(std::string_view text,
                                                                const std::filesystem::path& file);

[[nodiscard]] Result<ClassSet> parse_class_file(std::string_view text,
                                                const std::filesystem::path& file);

[[nodiscard]] Result<std::vector<KbEntry>> parse_kb_file(std::string_view text,
                                                         const std::filesystem::path& file);

}  // namespace myclean::rules
