#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/model/result.hpp"

// The Win32 sources of game detection (spike X5). Read-only: nothing here writes to the
// registry or a file, follows a link's target or hydrates a cloud file.
namespace myclean::platform {

// The launchers and the Game Bar store as one user sees them. `owner_sid` names the user whose
// hive is read from HKEY_USERS (the service, running as SYSTEM, for the owner); empty: the
// caller's own HKEY_CURRENT_USER.
class Win32GameSources {
 public:
  explicit Win32GameSources(std::wstring owner_sid = {}) : owner_sid_(std::move(owner_sid)) {}

  // SteamPath of the user's Software\Valve\Steam ("c:/program files (x86)/steam"); nullopt
  // when Steam is not installed for the user.
  [[nodiscard]] std::optional<std::filesystem::path> steam_path() const;

  // Steam's libraryfolders.vdf, every Epic manifest of
  // %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests, the path of every GOG game in
  // HKLM\SOFTWARE\WOW6432Node\GOG.com\Games and the fixed drives' roots. A launcher that is not
  // installed adds nothing; a file or key that cannot be read adds a problem.
  [[nodiscard]] games::LauncherData launcher_data() const;

  // Type and MatchedExeFullPath of every Children key of the user's System\GameConfigStore;
  // empty when the key does not exist (Game Bar never ran, or the hive is not loaded).
  [[nodiscard]] Result<std::vector<games::GameConfigEntry>> game_config_store() const;

 private:
  std::wstring owner_sid_;
};

// games::IGameFiles over Win32 file attributes and directory listings, which work on any file
// system (FAT and exFAT game drives included). Never opens a file.
class Win32GameFiles final : public games::IGameFiles {
 public:
  Win32GameFiles() = default;

  [[nodiscard]] bool is_directory(const std::filesystem::path& path) const override;
  [[nodiscard]] bool is_file(const std::filesystem::path& path) const override;
  [[nodiscard]] std::vector<std::filesystem::path> subfolders(
      const std::filesystem::path& path) const override;
};

// The roots ("C:\", "E:\") of the fixed drives. A USB SSD that Windows reports as fixed is one.
[[nodiscard]] std::vector<std::filesystem::path> fixed_drive_roots();

// SHQueryUserNotificationState for the calling user's session: meaningful only in the owner's
// interactive session (the tray), never in session 0. Shows nothing and needs no window.
[[nodiscard]] Result<games::NotificationState> query_user_notification_state();

}  // namespace myclean::platform
