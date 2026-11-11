#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"

// The owner's own game settings for the governor's game detection (GOV-6), from
// %ProgramData%\MyClean\games.toml, which only administrators may write:
//
//   owner = ["Diablo IV.exe", 'D:\Tools\Emulator\emu.exe']  # OwnerGameList entries
//   roots = ['D:\Games', 'E:\Itch']                         # library folders of other launchers
//
// Both keys are optional. The file lives outside the install folder, so a deploy keeps it.
namespace myclean::games {

struct OwnerGames {
  std::vector<std::string> owner;            // file names or full image paths (OwnerGameList)
  std::vector<std::filesystem::path> roots;  // absolute folders: everything below is a game
};

// Parses the text of games.toml. Fails with invalid_data, naming the field, for an unknown key, a
// value that is not a list of non-empty strings, or a root that is not an absolute drive path.
[[nodiscard]] Result<OwnerGames> parse_owner_games(std::string_view text);

}  // namespace myclean::games
