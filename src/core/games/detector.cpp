#include "core/games/detector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/games/paths.hpp"
#include "core/model/enum_names.hpp"
#include "core/os/clock.hpp"

namespace myclean::games {

namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 4> reason_names = {"owner_list", "library_root",
                                                          "game_config_store", "fullscreen_gpu"};

}  // namespace

std::string_view to_string(GameReason reason) {
  return detail::enum_name(reason_names, reason);
}

std::optional<GameReason> parse_game_reason(std::string_view text) {
  return detail::enum_from_name<GameReason>(reason_names, text);
}

OwnerGameList::OwnerGameList(std::span<const std::string> entries) {
  for (const std::string& entry : entries) {
    if (entry.empty()) {
      continue;
    }
    (entry.find_first_of("\\/") != std::string::npos ? paths_ : names_)
        .push_back(path_from_utf8(entry));
  }
}

std::optional<fs::path> OwnerGameList::match(const fs::path& image) const {
  for (const fs::path& path : paths_) {
    if (same_path(path, image)) {
      return path;
    }
  }
  const fs::path name = image.filename();
  for (const fs::path& entry : names_) {
    if (!name.empty() && same_path(entry, name)) {
      return entry;
    }
  }
  return std::nullopt;
}

GameDetector::GameDetector(const GameSources& sources, FullscreenGpuPolicy policy,
                           const os::IClock& clock)
    : fullscreen_(policy, clock) {
  update(sources);
}

void GameDetector::update(const GameSources& sources) {
  owner_games_ = OwnerGameList{sources.owner_games};
  roots_ = LibraryRoots{sources.roots};
  game_config_ = GameConfigStore{sources.game_config};
}

std::optional<GameVerdict> GameDetector::classify(const fs::path& image) const {
  if (std::optional<fs::path> entry = owner_games_.match(image)) {
    return GameVerdict{.reason = GameReason::owner_list, .evidence = std::move(*entry)};
  }
  if (const LibraryRoot* root = roots_.find(image)) {
    return GameVerdict{.reason = GameReason::library_root, .evidence = root->path};
  }
  if (game_config_.matches(image)) {
    return GameVerdict{.reason = GameReason::game_config_store, .evidence = image};
  }
  return std::nullopt;
}

std::optional<GameVerdict> GameDetector::verdict(std::uint32_t pid, const fs::path& image) const {
  if (std::optional<GameVerdict> known = classify(image)) {
    return known;
  }
  if (fullscreen_.detected(pid)) {
    return GameVerdict{.reason = GameReason::fullscreen_gpu};
  }
  return std::nullopt;
}

std::vector<fs::path> GameDetector::suggested_roots(const IGameFiles& files) const {
  std::vector<fs::path> folders;
  for (const fs::path& exe : game_config_.paths()) {
    const fs::path folder = exe.parent_path();
    if (roots_.find(exe) != nullptr || in_windows_apps(exe) || !folder.has_relative_path() ||
        std::ranges::any_of(folders,
                            [&folder](const fs::path& kept) { return same_path(kept, folder); }) ||
        !files.is_directory(folder)) {
      continue;
    }
    folders.push_back(folder);
  }
  std::vector<fs::path> suggestions;
  for (const fs::path& folder : folders) {
    if (std::ranges::none_of(folders, [&folder](const fs::path& other) {
          return !same_path(other, folder) && path_within(folder, other);
        })) {
      suggestions.push_back(folder);
    }
  }
  std::ranges::sort(suggestions, {}, [](const fs::path& path) { return path_key(path); });
  return suggestions;
}

}  // namespace myclean::games
