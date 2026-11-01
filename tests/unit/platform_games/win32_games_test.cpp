// The Win32 game sources against this machine: reads only, whether or not Steam, Epic or GOG
// are installed. The folder test writes only inside %TEMP%\myclean-it.

#include "platform/games/win32_games.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/games/detector.hpp"
#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/games/paths.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

// "E:\Jogos\Title\x.exe" -> "E:\Jogos": what the report may show of a path.
std::string drive_and_top(const fs::path& path) {
  fs::path shown = path.root_path();
  if (const fs::path relative = path.relative_path(); !relative.empty()) {
    shown /= *relative.begin();
  }
  return games::utf8(shown);
}

TEST(Win32GameSources, ReadsTheLaunchersOfThisMachine) {
  const Win32GameSources sources;
  const games::LauncherData data = sources.launcher_data();
  for (const std::string& problem : data.problems) {
    ADD_FAILURE() << problem;
  }
  const std::vector<fs::path> drives = fixed_drive_roots();
  EXPECT_EQ(data.drive_roots, drives);
  if (const std::optional<fs::path> steam = sources.steam_path()) {
    EXPECT_TRUE(steam->is_absolute());
  }

  const Win32GameFiles files;
  const games::RootsReport report = games::find_library_roots(data, {}, files);
  for (const std::string& problem : report.problems) {
    ADD_FAILURE() << problem;
  }
  std::array<std::size_t, 5> by_source{};
  for (const games::LibraryRoot& root : report.roots) {
    EXPECT_TRUE(files.is_directory(root.path)) << games::utf8(root.path);
    EXPECT_FALSE(games::in_windows_apps(root.path)) << games::utf8(root.path);
    ++by_source.at(static_cast<std::size_t>(root.source));
    std::cout << "[ info     ] root " << games::to_string(root.source) << " "
              << drive_and_top(root.path) << "\n";
  }
  std::cout << "[ info     ] steam " << (sources.steam_path() ? "installed" : "absent")
            << ", library files " << data.steam_library_files.size() << ", epic manifests "
            << data.epic_manifests.size() << ", gog folders " << data.gog_folders.size()
            << ", fixed drives " << drives.size() << "\n";
  std::cout << "[ info     ] roots: steam " << by_source.at(1) << ", epic " << by_source.at(2)
            << ", gog " << by_source.at(3) << ", xbox " << by_source.at(4) << "\n";
}

TEST(Win32GameSources, ReadsTheGameConfigStore) {
  const Result<std::vector<games::GameConfigEntry>> entries =
      Win32GameSources{}.game_config_store();
  ASSERT_TRUE(entries) << entries.error().message;
  const auto win32 = static_cast<std::size_t>(
      std::ranges::count(*entries, games::game_config_win32_type, &games::GameConfigEntry::type));
  const games::GameConfigStore store{*entries};
  for (const fs::path& path : store.paths()) {
    EXPECT_TRUE(path.is_absolute()) << games::utf8(path);
    EXPECT_TRUE(store.matches(path));
  }
  std::cout << "[ info     ] GameConfigStore: " << entries->size() << " entries, " << win32
            << " Type 1, " << store.paths().size() << " path-matched\n";

  const Win32GameFiles files;
  const games::RootsReport roots =
      games::find_library_roots(Win32GameSources{}.launcher_data(), {}, files);
  const os::SystemClock clock;
  const games::GameDetector detector{
      games::GameSources{.roots = roots.roots, .game_config = *entries},
      games::FullscreenGpuPolicy{}, clock};
  const auto installed = static_cast<std::size_t>(std::ranges::count_if(
      store.paths(), [&files](const fs::path& path) { return files.is_file(path); }));
  const auto in_roots = static_cast<std::size_t>(std::ranges::count_if(
      store.paths(),
      [&detector](const fs::path& path) { return detector.roots().find(path) != nullptr; }));
  std::cout << "[ info     ] path-matched images present " << installed << ", under a library root "
            << in_roots << "\n";
  for (const fs::path& folder : detector.suggested_roots(files)) {
    std::cout << "[ info     ] suggested root under " << drive_and_top(folder) << "\n";
  }
}

TEST(Win32GameSources, UnknownOwnerReadsNothing) {
  const Win32GameSources sources{L"S-1-5-21-1-2-3-999999"};
  EXPECT_EQ(sources.steam_path(), std::nullopt);
  const Result<std::vector<games::GameConfigEntry>> entries = sources.game_config_store();
  ASSERT_TRUE(entries) << entries.error().message;
  EXPECT_TRUE(entries->empty());
}

TEST(Win32GameFiles, DescribesFoldersAndFiles) {
  const fs::path root = fs::temp_directory_path() / "myclean-it" / "platform-games";
  std::error_code ignored;
  fs::remove_all(root, ignored);
  fs::create_directories(root / "XboxGames" / "Title" / "Content");
  fs::create_directories(root / "XboxGames" / "Other");
  std::ofstream{root / "XboxGames" / "Title" / "Content" / "MicrosoftGame.config"} << "<Game/>";

  const Win32GameFiles files;
  EXPECT_TRUE(files.is_directory(root / "XboxGames"));
  EXPECT_FALSE(files.is_file(root / "XboxGames"));
  EXPECT_TRUE(files.is_file(root / "XboxGames" / "Title" / "Content" / "MicrosoftGame.config"));
  EXPECT_FALSE(files.is_directory(root / "missing"));
  EXPECT_FALSE(files.is_file(root / "missing"));
  std::vector<fs::path> titles = files.subfolders(root / "XboxGames");
  std::ranges::sort(titles);
  EXPECT_EQ(titles,
            (std::vector<fs::path>{root / "XboxGames" / "Other", root / "XboxGames" / "Title"}));
  EXPECT_TRUE(files.subfolders(root / "missing").empty());

  // The drive root of the test folder, treated as a drive holding XboxGames.
  const games::RootsReport report =
      games::find_library_roots(games::LauncherData{.drive_roots = {root}}, {}, files);
  ASSERT_EQ(report.roots.size(), 1U);
  EXPECT_EQ(report.roots.front().path, root / "XboxGames");
  fs::remove_all(root, ignored);
}

TEST(NotificationState, IsReadForThisSession) {
  const Result<games::NotificationState> state = query_user_notification_state();
  ASSERT_TRUE(state) << state.error().message;
  std::cout << "[ info     ] notification state " << games::to_string(*state) << "\n";
}

}  // namespace
}  // namespace myclean::platform
