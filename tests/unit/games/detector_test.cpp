#include "core/games/detector.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::games {
namespace {

namespace fs = std::filesystem;

constexpr auto python_store =
    LR"(C:\Program Files\WindowsApps\PythonSoftwareFoundation.Python.3.12_3.12.2800.0_x64__)"
    LR"(qbz5n2kfra8p0\python3.12.exe)";

// The owner's machine as spike X5 found it, reduced to one entry of each kind.
GameSources owner_machine() {
  return GameSources{
      .owner_games = {"Albion-Online.exe", R"(G:\UE\Title\Binaries\Win64\Title.exe)"},
      .roots = {{.path = LR"(E:\Jogos)", .source = RootSource::owner},
                {.path = LR"(E:\SteamLibrary\steamapps\common)", .source = RootSource::steam}},
      .game_config =
          {
              // Path-matched (Type 1) entries of games that ran here.
              {.type = 1, .matched_exe_full_path = LR"(E:\Jogos\Diablo IV\Diablo IV.exe)"},
              {.type = 1,
               .matched_exe_full_path =
                   LR"(C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2\PathOfExile.exe)"},
              {.type = 1,
               .matched_exe_full_path =
                   LR"(E:\Arena Breakout Infinite\Win64\ArenaBreakoutInfinite.exe)"},
              {.type = 1, .matched_exe_full_path = LR"(E:\Netmarble Game\Launcher\Netmarble.exe)"},
              {.type = 1,
               .matched_exe_full_path = LR"(E:\Netmarble Game\Title\Binaries\Title.exe)"},
              {.type = 1, .matched_exe_full_path = LR"(E:\Gone\gone.exe)"},  // uninstalled
              {.type = 1, .matched_exe_full_path = LR"(E:\root.exe)"},
              // Name-only: no path.
              {.type = 1},
              // Packaged (Type 2): Store Python and winget are not games.
              {.type = 2, .matched_exe_full_path = python_store},
              {.type = 2,
               .matched_exe_full_path = LR"(C:\Program Files\WindowsApps\Microsoft.)"
                                        LR"(DesktopAppInstaller_8wekyb3d8bbwe\winget.exe)"},
          },
  };
}

class Detector : public ::testing::Test {
 protected:
  [[nodiscard]] GameDetector& detector() { return detector_; }

  // Observes `seconds` + 1 seconds of `pid` in borderless fullscreen with `gpu` % 3D use.
  void fullscreen_for(int seconds, std::uint32_t pid, double gpu) {
    for (int second = 0; second <= seconds; ++second) {
      detector_.fullscreen().observe(
          {.state = NotificationState::busy, .foreground_pid = pid, .gpu_3d_percent = gpu});
      clock_.advance(std::chrono::seconds{1});
    }
  }

 private:
  fakes::FakeClock clock_{WallTime{}};
  GameDetector detector_{owner_machine(), FullscreenGpuPolicy{}, clock_};
};

TEST_F(Detector, OwnerRootGameDetected) {
  EXPECT_EQ(detector().classify(LR"(E:\Jogos\Title\title.exe)"),
            (GameVerdict{.reason = GameReason::library_root, .evidence = LR"(E:\Jogos)"}));
}

TEST_F(Detector, SteamLibraryGameDetected) {
  EXPECT_EQ(detector().classify(LR"(e:\steamlibrary\steamapps\common\Palworld\Pal\Binaries\)"
                                LR"(Win64\Palworld-Win64-Shipping.exe)"),
            (GameVerdict{.reason = GameReason::library_root,
                         .evidence = LR"(E:\SteamLibrary\steamapps\common)"}));
}

TEST_F(Detector, StorePythonIsNotAGame) {
  EXPECT_EQ(detector().classify(python_store), std::nullopt);
  EXPECT_EQ(detector().verdict(1234, python_store), std::nullopt);
}

TEST_F(Detector, PathMatchedGameConfigEntryIsAGame) {
  const fs::path image =
      LR"(c:\program files (x86)\grinding gear games\path of exile 2\PATHOFEXILE.EXE)";
  EXPECT_EQ(detector().classify(image),
            (GameVerdict{.reason = GameReason::game_config_store, .evidence = image}));
  // Only that image: another program in the same folder is not.
  EXPECT_EQ(detector().classify(
                LR"(C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2\Uninstall.exe)"),
            std::nullopt);
}

TEST_F(Detector, OwnerListByNameAndByPath) {
  EXPECT_EQ(detector().classify(LR"(E:\Albion\game\albion-online.exe)"),
            (GameVerdict{.reason = GameReason::owner_list, .evidence = "Albion-Online.exe"}));
  EXPECT_EQ(detector().classify(LR"(g:\ue\title\binaries\win64\title.exe)"),
            (GameVerdict{.reason = GameReason::owner_list,
                         .evidence = R"(G:\UE\Title\Binaries\Win64\Title.exe)"}));
  EXPECT_EQ(detector().classify(LR"(G:\UE\Other\Binaries\Win64\Title2.exe)"), std::nullopt);
}

TEST_F(Detector, OwnerListWinsOverOtherReasons) {
  detector().update(GameSources{.owner_games = {"Diablo IV.exe"},
                                .roots = owner_machine().roots,
                                .game_config = owner_machine().game_config});
  EXPECT_EQ(detector().classify(LR"(E:\Jogos\Diablo IV\Diablo IV.exe)"),
            (GameVerdict{.reason = GameReason::owner_list, .evidence = "Diablo IV.exe"}));
}

TEST_F(Detector, UnrealEngineFromAnEngineManifestIsNotAGame) {
  // The engine's manifest adds no root (EpicManifest and Roots tests), so its editor is no game.
  EXPECT_EQ(detector().classify(LR"(W:\UE_5.4\Engine\Binaries\Win64\UnrealEditor.exe)"),
            std::nullopt);
}

TEST_F(Detector, FullscreenGpuGameDetected) {
  const fs::path image = LR"(D:\Somewhere\unlisted.exe)";
  EXPECT_EQ(detector().verdict(4120, image), std::nullopt);
  fullscreen_for(10, 4120, 15.0);
  EXPECT_EQ(detector().classify(image), std::nullopt);
  EXPECT_EQ(detector().verdict(4120, image), GameVerdict{.reason = GameReason::fullscreen_gpu});
  // An update of the sources keeps the fullscreen verdict.
  detector().update(GameSources{});
  EXPECT_EQ(detector().verdict(4120, image), GameVerdict{.reason = GameReason::fullscreen_gpu});
}

TEST_F(Detector, FullscreenVideoPlayerIsNotAGame) {
  fullscreen_for(30, 900, 0.1);
  EXPECT_EQ(detector().verdict(900, LR"(C:\Program Files\VideoLAN\VLC\vlc.exe)"), std::nullopt);
}

TEST_F(Detector, SuggestsFoldersOfGamesOutsideEveryRoot) {
  fakes::FakeFileSystem files;
  files.add_directory(LR"(E:\Jogos\Diablo IV)");
  files.add_directory(LR"(C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2)");
  files.add_directory(LR"(E:\Arena Breakout Infinite\Win64)");
  files.add_directory(LR"(E:\Netmarble Game\Launcher)");
  files.add_directory(LR"(E:\Netmarble Game\Title\Binaries)");
  const std::vector<fs::path> expected = {
      LR"(C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2)",
      LR"(E:\Arena Breakout Infinite\Win64)", LR"(E:\Netmarble Game\Launcher)",
      LR"(E:\Netmarble Game\Title\Binaries)"};
  EXPECT_EQ(detector().suggested_roots(FileSystemGameFiles{files}), expected);

  // A folder inside another suggestion is left out.
  detector().update(GameSources{
      .game_config = {{.type = 1, .matched_exe_full_path = LR"(E:\Albion\Albion.exe)"},
                      {.type = 1, .matched_exe_full_path = LR"(E:\Albion\game\Albion-Online.exe)"},
                      {.type = 1, .matched_exe_full_path = LR"(e:\albion\launcher.exe)"}}});
  files.add_directory(LR"(E:\Albion\game)");
  EXPECT_EQ(detector().suggested_roots(FileSystemGameFiles{files}),
            std::vector<fs::path>{LR"(E:\Albion)"});
}

TEST_F(Detector, GameConfigStoreKeepsPathMatchedEntriesOnly) {
  const std::vector<fs::path> expected = {
      LR"(E:\Jogos\Diablo IV\Diablo IV.exe)",
      LR"(C:\Program Files (x86)\Grinding Gear Games\Path of Exile 2\PathOfExile.exe)",
      LR"(E:\Arena Breakout Infinite\Win64\ArenaBreakoutInfinite.exe)",
      LR"(E:\Netmarble Game\Launcher\Netmarble.exe)",
      LR"(E:\Netmarble Game\Title\Binaries\Title.exe)",
      LR"(E:\Gone\gone.exe)",
      LR"(E:\root.exe)"};
  EXPECT_EQ(detector().game_config().paths(), expected);
  EXPECT_FALSE(detector().game_config().matches(python_store));
}

TEST(GameReasonNames, AreFixed) {
  EXPECT_EQ(to_string(GameReason::owner_list), "owner_list");
  EXPECT_EQ(to_string(GameReason::library_root), "library_root");
  EXPECT_EQ(to_string(GameReason::game_config_store), "game_config_store");
  EXPECT_EQ(to_string(GameReason::fullscreen_gpu), "fullscreen_gpu");
  EXPECT_EQ(parse_game_reason("fullscreen_gpu"), GameReason::fullscreen_gpu);
  EXPECT_EQ(parse_game_reason("game"), std::nullopt);
}

}  // namespace
}  // namespace myclean::games
