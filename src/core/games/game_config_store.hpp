#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <vector>

// The owner's Game Bar registrations, HKU\<owner SID>\System\GameConfigStore\Children\* (spike
// X5 §1). Only path-matched entries are trusted: Type 2 (packaged) entries include Store Python
// and winget, and name-only entries use generic names such as fm.exe.
namespace myclean::games {

// Type of a Win32 executable entry; 2 is a packaged app named by its AUMID.
inline constexpr std::uint32_t game_config_win32_type = 1;

// One Children\{GUID} key as read.
struct GameConfigEntry {
  std::uint32_t type = 0;                       // Type (DWORD); 0 when absent
  std::filesystem::path matched_exe_full_path;  // MatchedExeFullPath; empty when absent
};

// The path-matched entries: Type 1 with a MatchedExeFullPath.
class GameConfigStore {
 public:
  GameConfigStore() = default;
  explicit GameConfigStore(std::span<const GameConfigEntry> entries);

  // True when `image` equals a kept MatchedExeFullPath, ignoring ASCII case.
  [[nodiscard]] bool matches(const std::filesystem::path& image) const;
  // The kept paths, in entry order, without duplicates.
  [[nodiscard]] const std::vector<std::filesystem::path>& paths() const { return paths_; }

 private:
  std::vector<std::filesystem::path> paths_;
  std::set<std::wstring> keys_;  // path_key of each path
};

}  // namespace myclean::games
