#include "core/games/game_config_store.hpp"

#include <filesystem>
#include <span>

#include "core/games/paths.hpp"

namespace myclean::games {

GameConfigStore::GameConfigStore(std::span<const GameConfigEntry> entries) {
  for (const GameConfigEntry& entry : entries) {
    if (entry.type != game_config_win32_type || !entry.matched_exe_full_path.is_absolute()) {
      continue;
    }
    if (keys_.insert(path_key(entry.matched_exe_full_path)).second) {
      paths_.push_back(entry.matched_exe_full_path);
    }
  }
}

bool GameConfigStore::matches(const std::filesystem::path& image) const {
  return keys_.contains(path_key(image));
}

}  // namespace myclean::games
