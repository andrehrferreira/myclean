#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/packs/schema.hpp"

// Parsers of the pack files this module defines (schema.hpp). Each validates the whole file and
// returns either all of it or one Errc::invalid_data error naming the file, the entry (by
// position and id) and the field. Case ids are returned as written, not yet namespaced.
namespace myclean::packs {

[[nodiscard]] Result<Manifest> parse_manifest(std::string_view text,
                                              const std::filesystem::path& file);

[[nodiscard]] Result<std::vector<RelocationRecipe>> parse_relocate_file(
    std::string_view text, const std::filesystem::path& file);

[[nodiscard]] Result<std::vector<CompressTarget>> parse_compress_file(
    std::string_view text, const std::filesystem::path& file);

}  // namespace myclean::packs
