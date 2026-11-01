#include "core/games/epic.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::games {
namespace {

namespace fs = std::filesystem;

// Shapes of real manifests (spike X5 §2), trimmed to the fields that matter.
constexpr std::string_view game_manifest = R"json({
  "FormatVersion": 0,
  "bIsIncompleteInstall": false,
  "LaunchExecutable": "FortniteGame/Binaries/Win64/FortniteLauncher.exe",
  "DisplayName": "Fortnite",
  "InstallLocation": "C:\\Program Files\\Epic Games\\Fortnite",
  "AppCategories": ["public", "games", "applications"],
  "bIsApplication": true
})json";

constexpr std::string_view engine_manifest = R"json({
  "LaunchExecutable": "Engine/Binaries/Win64/UnrealEditor.exe",
  "DisplayName": "Unreal Engine",
  "InstallLocation": "W:\\UE_5.4",
  "AppCategories": ["engines/ue5", "engines"]
})json";

constexpr std::string_view plugin_manifest = R"json({
  "LaunchExecutable": "",
  "DisplayName": "MetaHuman plugin",
  "InstallLocation": "W:\\UE_5.4\\Engine\\Plugins\\Marketplace\\MetaHuman",
  "AppCategories": []
})json";

TEST(EpicManifest, ReadsTheFields) {
  const Result<EpicManifest> manifest = parse_epic_manifest(game_manifest);
  ASSERT_TRUE(manifest) << manifest.error().message;
  EXPECT_EQ(manifest->display_name, "Fortnite");
  EXPECT_EQ(manifest->install_location, fs::path{LR"(C:\Program Files\Epic Games\Fortnite)"});
  EXPECT_EQ(manifest->launch_executable, "FortniteGame/Binaries/Win64/FortniteLauncher.exe");
  EXPECT_EQ(manifest->categories, (std::vector<std::string>{"public", "games", "applications"}));
  EXPECT_TRUE(is_epic_game(*manifest));
}

TEST(EpicManifest, UnrealEngineIsNotAGame) {
  const Result<EpicManifest> engine = parse_epic_manifest(engine_manifest);
  ASSERT_TRUE(engine) << engine.error().message;
  EXPECT_FALSE(is_epic_game(*engine));
  // Not even when it is also tagged as a game.
  EpicManifest tagged = *engine;
  tagged.categories.emplace_back("games");
  EXPECT_FALSE(is_epic_game(tagged));
}

TEST(EpicManifest, PluginsAndAppsWithoutLaunchExecutableAreNotGames) {
  const Result<EpicManifest> plugin = parse_epic_manifest(plugin_manifest);
  ASSERT_TRUE(plugin) << plugin.error().message;
  EXPECT_FALSE(is_epic_game(*plugin));
  EXPECT_FALSE(is_epic_game(
      EpicManifest{.launch_executable = "x.exe", .categories = {"games", "plugins/engine"}}));
  EXPECT_FALSE(is_epic_game(EpicManifest{.categories = {"games"}}));
  EXPECT_FALSE(is_epic_game(EpicManifest{.launch_executable = "x.exe", .categories = {"gamesx"}}));
}

TEST(EpicManifest, MissingFieldsAreEmpty) {
  const Result<EpicManifest> manifest = parse_epic_manifest(R"({"DisplayName": null})");
  ASSERT_TRUE(manifest) << manifest.error().message;
  EXPECT_TRUE(manifest->install_location.empty());
  EXPECT_TRUE(manifest->categories.empty());
  EXPECT_FALSE(is_epic_game(*manifest));
}

TEST(EpicManifest, RejectsMalformedManifests) {
  for (const std::string_view text :
       {std::string_view{"{"}, std::string_view{"[1]"}, std::string_view{R"({"DisplayName": 3})"},
        std::string_view{R"({"AppCategories": "games"})"},
        std::string_view{R"({"AppCategories": ["games", 1]})"}}) {
    const Result<EpicManifest> manifest = parse_epic_manifest(text);
    ASSERT_FALSE(manifest) << text;
    EXPECT_EQ(manifest.error().code, Errc::invalid_data);
  }
}

}  // namespace
}  // namespace myclean::games
