#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/os/clock.hpp"

// Game detection (spec Game Detection): combines the owner's game list, library roots, the
// owner's GameConfigStore and the fullscreen + GPU signal into one verdict with its reason.
namespace myclean::games {

// Why a process is a game, strongest first; the first that applies is reported.
enum class GameReason { owner_list, library_root, game_config_store, fullscreen_gpu };

[[nodiscard]] std::string_view to_string(GameReason reason);
[[nodiscard]] std::optional<GameReason> parse_game_reason(std::string_view text);

struct GameVerdict {
  GameReason reason = GameReason::owner_list;
  // owner_list: the list entry; library_root: the root; game_config_store: the
  // MatchedExeFullPath; fullscreen_gpu: empty.
  std::filesystem::path evidence;

  friend bool operator==(const GameVerdict&, const GameVerdict&) = default;
};

// The owner's game list. An entry with a '\' or '/' is a full image path; any other is a file
// name ("Diablo IV.exe") matched against the image's file name. Both ignore ASCII case.
class OwnerGameList {
 public:
  OwnerGameList() = default;
  explicit OwnerGameList(std::span<const std::string> entries);

  // The entry naming `image`, as written; nullopt when none does.
  [[nodiscard]] std::optional<std::filesystem::path> match(
      const std::filesystem::path& image) const;

 private:
  std::vector<std::filesystem::path> paths_;
  std::vector<std::filesystem::path> names_;
};

// What detection is built from: the owner's settings and what the platform read.
struct GameSources {
  std::vector<std::string> owner_games;      // OwnerGameList entries
  std::vector<LibraryRoot> roots;            // find_library_roots(...).roots
  std::vector<GameConfigEntry> game_config;  // every GameConfigStore Children entry
};

// Not thread-safe.
class GameDetector {
 public:
  // `clock` must outlive the detector.
  GameDetector(const GameSources& sources, FullscreenGpuPolicy policy, const os::IClock& clock);

  // New sources (a launcher installed a game, the owner edited a list). Fullscreen verdicts are
  // kept.
  void update(const GameSources& sources);

  // The verdict from the image alone, as at process start: owner list, then library root, then
  // a path-matched GameConfigStore entry. nullopt: not known to be a game.
  [[nodiscard]] std::optional<GameVerdict> classify(const std::filesystem::path& image) const;
  // classify(), else fullscreen_gpu when the fullscreen detector has detected `pid`.
  [[nodiscard]] std::optional<GameVerdict> verdict(std::uint32_t pid,
                                                   const std::filesystem::path& image) const;

  // Fed once per second with the tray's state, the foreground PID and its GPU use; forget() on
  // process exit.
  [[nodiscard]] FullscreenGpuDetector& fullscreen() { return fullscreen_; }
  [[nodiscard]] const FullscreenGpuDetector& fullscreen() const { return fullscreen_; }
  [[nodiscard]] const LibraryRoots& roots() const { return roots_; }
  [[nodiscard]] const GameConfigStore& game_config() const { return game_config_; }

  // Owner roots to suggest (spike X5, implication 5): the folders, existing in `files`, of the
  // path-matched GameConfigStore entries that lie outside every library root and WindowsApps.
  // Volume roots and folders inside another suggestion are left out; sorted. Suggestions only:
  // they become roots when the owner confirms them.
  [[nodiscard]] std::vector<std::filesystem::path> suggested_roots(const IGameFiles& files) const;

 private:
  OwnerGameList owner_games_;
  LibraryRoots roots_;
  GameConfigStore game_config_;
  FullscreenGpuDetector fullscreen_;
};

}  // namespace myclean::games
