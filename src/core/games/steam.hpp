#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

// Steam's library folders, from libraryfolders.vdf (spike X5 §2).
namespace myclean::games {

// <steam>\steamapps\libraryfolders.vdf, for Steam installed in `steam` (the SteamPath value of
// HKCU\Software\Valve\Steam, which uses '/' separators).
[[nodiscard]] std::filesystem::path library_folders_file(const std::filesystem::path& steam);

// The library folders that libraryfolders.vdf (Valve KeyValues text) lists: the "path" of every
// entry of the current format ("0" { "path" "E:\\SteamLibrary" ... }) and the value of every
// numbered key of the old one ("1" "E:\\SteamLibrary"), in file order. Entries may name folders
// that no longer exist. Fails with invalid_data, naming the line, on malformed text.
[[nodiscard]] Result<std::vector<std::filesystem::path>> parse_library_folders(
    std::string_view vdf);

// The folder that holds a library's games: <library>\steamapps\common.
[[nodiscard]] std::filesystem::path steam_common_folder(const std::filesystem::path& library);

}  // namespace myclean::games
