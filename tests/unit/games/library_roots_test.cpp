#include "core/games/library_roots.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/games/paths.hpp"
#include "core/model/units.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::games {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view steam_vdf = R"vdf("libraryfolders"
{
	"0" { "path" "C:\\Program Files (x86)\\Steam" }
	"1" { "path" "E:\\SteamLibrary" }
	"2" { "path" "L:\\SteamLibrary" }
	"3" { "path" "W:\\SteamLibrary" }
}
)vdf";

constexpr std::string_view epic_game = R"json({"LaunchExecutable": "Game.exe",
  "InstallLocation": "D:\\Epic Games\\SomeGame", "AppCategories": ["public", "games"]})json";
constexpr std::string_view epic_engine = R"json({"LaunchExecutable": "Engine/UnrealEditor.exe",
  "InstallLocation": "W:\\UE_5.4", "AppCategories": ["engines/ue5", "engines"]})json";
constexpr std::string_view epic_removed = R"json({"LaunchExecutable": "Game.exe",
  "InstallLocation": "D:\\Epic Games\\Removed", "AppCategories": ["games"]})json";

TEST(Paths, CompareCaseInsensitivelyAtFolderBoundaries) {
  EXPECT_TRUE(same_path(fs::path{LR"(E:\Jogos\X.exe)"}, fs::path{LR"(e:\JOGOS\x.EXE)"}));
  EXPECT_TRUE(same_path(fs::path{LR"(\\?\E:\Jogos\)"}, fs::path{"E:/Jogos"}));
  EXPECT_TRUE(path_within(fs::path{LR"(e:\jogos\diablo iv\Diablo IV.exe)"}, fs::path{"E:\\Jogos"}));
  EXPECT_TRUE(path_within(fs::path{LR"(E:\Jogos)"}, fs::path{LR"(E:\Jogos\)"}));
  EXPECT_TRUE(path_within(fs::path{LR"(E:\x.exe)"}, fs::path{LR"(E:\)"}));
  EXPECT_FALSE(path_within(fs::path{LR"(E:\Jogos2\x.exe)"}, fs::path{LR"(E:\Jogos)"}));
  EXPECT_FALSE(path_within(fs::path{LR"(E:\x.exe)"}, fs::path{}));
  EXPECT_TRUE(path_within(fs::path{LR"(\\?\UNC\nas\games\x.exe)"}, fs::path{LR"(\\nas\games)"}));
  EXPECT_TRUE(in_windows_apps(fs::path{LR"(C:\Program Files\WindowsApps\Python\python.exe)"}));
  EXPECT_TRUE(in_windows_apps(fs::path{LR"(E:\windowsapps\x.exe)"}));
  EXPECT_FALSE(in_windows_apps(fs::path{LR"(E:\WindowsAppsBackup\x.exe)"}));
}

class Roots : public ::testing::Test {
 protected:
  Roots() {
    files_.add_directory(LR"(C:\Program Files (x86)\Steam\steamapps\common)");
    files_.add_directory(LR"(E:\SteamLibrary\steamapps\common\Palworld)");
    files_.add_directory(LR"(L:\SteamLibrary)");  // no steamapps: a stale entry
    files_.add_directory(LR"(D:\Epic Games\SomeGame)");
    files_.add_directory(LR"(W:\UE_5.4\Engine)");
    files_.add_directory(LR"(G:\GOG Games\Witcher)");
  }

  [[nodiscard]] RootsReport find(const LauncherData& data,
                                 const std::vector<fs::path>& owner = {}) const {
    return find_library_roots(data, owner, game_files_);
  }

  [[nodiscard]] fakes::FakeFileSystem& files() { return files_; }

 private:
  fakes::FakeFileSystem files_;
  FileSystemGameFiles game_files_{files_};
};

TEST_F(Roots, SteamRootsAreTheExistingCommonFolders) {
  const RootsReport report = find(LauncherData{.steam_library_files = {std::string(steam_vdf)}});
  EXPECT_TRUE(report.problems.empty());
  const std::vector<LibraryRoot> expected = {
      {.path = LR"(C:\Program Files (x86)\Steam\steamapps\common)", .source = RootSource::steam},
      {.path = LR"(E:\SteamLibrary\steamapps\common)", .source = RootSource::steam}};
  EXPECT_EQ(report.roots, expected);
}

TEST_F(Roots, EpicRootsAreExistingGamesOnly) {
  const RootsReport report =
      find(LauncherData{.epic_manifests = {std::string(epic_game), std::string(epic_engine),
                                           std::string(epic_removed), "not json"}});
  const std::vector<LibraryRoot> expected = {
      {.path = LR"(D:\Epic Games\SomeGame)", .source = RootSource::epic}};
  EXPECT_EQ(report.roots, expected);
  ASSERT_EQ(report.problems.size(), 1U);
  EXPECT_EQ(report.problems.front(), "Epic manifest: not a JSON object");
}

TEST_F(Roots, GogFoldersThatExist) {
  const RootsReport report = find(LauncherData{
      .gog_folders = {LR"(G:\GOG Games\Witcher)", LR"(G:\GOG Games\Gone)", "relative"}});
  const std::vector<LibraryRoot> expected = {
      {.path = LR"(G:\GOG Games\Witcher)", .source = RootSource::gog}};
  EXPECT_EQ(report.roots, expected);
}

TEST_F(Roots, XboxGamesNeedsATitleWithMicrosoftGameConfig) {
  files().add_file(LR"(C:\XboxGames\Forza\Content\MicrosoftGame.config)", Bytes{100});
  files().add_file(LR"(D:\XboxGames\Halo\MicrosoftGame.config)", Bytes{100});
  files().add_directory(LR"(F:\XboxGames\Empty)");
  files().add_directory(LR"(K:\XboxGames\Linked)", {.reparse_point = true});
  const RootsReport report =
      find(LauncherData{.drive_roots = {"C:\\", "D:\\", "E:\\", "F:\\", "K:\\"}});
  const std::vector<LibraryRoot> expected = {
      {.path = LR"(C:\XboxGames)", .source = RootSource::xbox},
      {.path = LR"(D:\XboxGames)", .source = RootSource::xbox}};
  EXPECT_EQ(report.roots, expected);
}

TEST_F(Roots, OwnerRootsComeFirstAndNeedNotExist) {
  const RootsReport report =
      find(LauncherData{.steam_library_files = {std::string(steam_vdf)},
                        .problems = {"GOG: access denied"}},
           {LR"(E:\Jogos)", LR"(E:\SteamLibrary\steamapps\common)", "Jogos"});
  const std::vector<LibraryRoot> expected = {
      {.path = LR"(E:\Jogos)", .source = RootSource::owner},
      {.path = LR"(E:\SteamLibrary\steamapps\common)", .source = RootSource::owner},
      {.path = LR"(C:\Program Files (x86)\Steam\steamapps\common)", .source = RootSource::steam}};
  EXPECT_EQ(report.roots, expected);
  const std::vector<std::string> problems = {"GOG: access denied",
                                             "owner root is not an absolute path: Jogos"};
  EXPECT_EQ(report.problems, problems);
}

TEST_F(Roots, MalformedLibraryFileIsAProblem) {
  const RootsReport report = find(LauncherData{.steam_library_files = {"\"libraryfolders\" {"}});
  EXPECT_TRUE(report.roots.empty());
  ASSERT_EQ(report.problems.size(), 1U);
  EXPECT_EQ(report.problems.front(), "libraryfolders.vdf: line 1: missing '}'");
}

TEST(LibraryRootsMatch, OwnerRootHoldsItsGames) {
  const std::vector<LibraryRoot> roots = {{.path = LR"(E:\Jogos)", .source = RootSource::owner}};
  const LibraryRoots matcher{roots};
  const LibraryRoot* root = matcher.find(LR"(E:\Jogos\Title\title.exe)");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->path, fs::path{LR"(E:\Jogos)"});
  EXPECT_NE(matcher.find(LR"(e:\JOGOS\title\TITLE.EXE)"), nullptr);
  EXPECT_EQ(matcher.find(LR"(E:\Jogos2\title.exe)"), nullptr);
  EXPECT_EQ(matcher.find(LR"(E:\Albion\game\Albion-Online.exe)"), nullptr);
}

TEST(LibraryRootsMatch, WindowsAppsIsNeverALibrary) {
  const std::vector<LibraryRoot> roots = {
      {.path = LR"(C:\Program Files\WindowsApps)", .source = RootSource::owner},
      {.path = LR"(E:\)", .source = RootSource::owner},
      {.path = "relative", .source = RootSource::owner},
      {.path = LR"(e:\)", .source = RootSource::steam}};
  const LibraryRoots matcher{roots};
  ASSERT_EQ(matcher.roots().size(), 1U);
  EXPECT_EQ(matcher.roots().front().path, fs::path{LR"(E:\)"});
  EXPECT_EQ(matcher.find(LR"(C:\Program Files\WindowsApps\PythonSoftwareFoundation.Python.3.12_)"
                         LR"(3.12.2800.0_x64__qbz5n2kfra8p0\python3.12.exe)"),
            nullptr);
  EXPECT_EQ(matcher.find(LR"(E:\WindowsApps\Microsoft.DesktopAppInstaller\winget.exe)"), nullptr);
  EXPECT_NE(matcher.find(LR"(E:\Albion\game\Albion-Online.exe)"), nullptr);
}

TEST(RootSourceNames, AreFixed) {
  EXPECT_EQ(to_string(RootSource::owner), "owner");
  EXPECT_EQ(to_string(RootSource::xbox), "xbox");
}

}  // namespace
}  // namespace myclean::games
