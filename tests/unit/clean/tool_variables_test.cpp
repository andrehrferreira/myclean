#include "core/clean/tool_variables.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "clean_fakes.hpp"
#include "core/model/result.hpp"

namespace myclean::clean {
namespace {

namespace fs = std::filesystem;
using testing::FakeEnvironment;

// Files in memory, by exact path; any other path is not_found.
ToolFiles files_of(std::optional<fs::path> steam, std::map<fs::path, Result<std::string>> files) {
  return ToolFiles{.steam = std::move(steam),
                   // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc
                   .read = [files = std::move(files)](const fs::path& path) -> Result<std::string> {
                     const auto found = files.find(path);
                     return found == files.end() ? fail(Errc::not_found, "no such file")
                                                 : found->second;
                   }};
}

const fs::path steam = R"(C:\Program Files (x86)\Steam)";
const fs::path vdf = R"(C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf)";
const fs::path settings_store = R"(C:\Users\owner\AppData\Roaming\Docker\settings-store.json)";
const fs::path legacy_settings = R"(C:\Users\owner\AppData\Roaming\Docker\settings.json)";
const std::wstring default_docker = LR"(C:\Users\owner\AppData\Local\Docker\wsl)";

// The owner's %APPDATA% and %LOCALAPPDATA%.
class ToolVariable : public ::testing::Test {
 protected:
  ToolVariable() {
    owner_.set(L"APPDATA", LR"(C:\Users\owner\AppData\Roaming)");
    owner_.set(L"LOCALAPPDATA", LR"(C:\Users\owner\AppData\Local)");
  }

  [[nodiscard]] const FakeEnvironment& owner() const { return owner_; }

 private:
  FakeEnvironment owner_;
};

// The reference machine's libraryfolders.vdf: Steam's own folder and libraries on other drives,
// one written with a trailing separator and '/'.
TEST_F(ToolVariable, SteamLibraryIsEveryLibraryOfLibraryFolders) {
  const ToolFiles files = files_of(steam, {{vdf, R"("libraryfolders"
{
  "0" { "path" "C:\\Program Files (x86)\\Steam" "label" "" }
  "1" { "path" "E:\\SteamLibrary" "apps" { "275850" "33478918624" } }
  "2" { "path" "D:/Games/Steam Library/" }
})"}});

  EXPECT_EQ(tool_variable(L"steam_library", owner(), files),
            (std::vector<std::wstring>{LR"(C:\Program Files (x86)\Steam)", LR"(E:\SteamLibrary)",
                                       LR"(D:\Games\Steam Library)"}));
}

// Without Steam, or when its file cannot be read or parsed, the variable has no value: a target
// naming it selects nothing.
TEST_F(ToolVariable, SteamLibraryHasNoValueWithoutAReadableFile) {
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", owner(), files_of(std::nullopt, {})),
            std::vector<std::wstring>{});
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", owner(), files_of(steam, {})),
            std::vector<std::wstring>{});
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", owner(),
                          files_of(steam, {{vdf, fail(Errc::access_denied, "denied")}})),
            std::vector<std::wstring>{});
  EXPECT_EQ(
      tool_variable(L"STEAM_LIBRARY", owner(), files_of(steam, {{vdf, R"("libraryfolders" {)"}})),
      std::vector<std::wstring>{});
}

// The reference machine: settings-store.json names the live folder on D:, the legacy
// settings.json the folder before its rename on F:. Both are kept, and Docker's default.
TEST_F(ToolVariable, DockerWslDataIsEveryFolderTheSettingsName) {
  const ToolFiles files = files_of(
      std::nullopt,
      {{settings_store,
        R"({"AutoStart": false, "CustomWslDistroDir": "D:\\Database\\DockerDesktopWSL"})"},
       {legacy_settings, R"({"customWslDistroDir": "F:\\WSL\\wsl\\DockerDesktopWSL\\"})"}});

  EXPECT_EQ(tool_variable(L"DOCKER_WSL_DATA", owner(), files),
            (std::vector<std::wstring>{LR"(D:\Database\DockerDesktopWSL)",
                                       LR"(F:\WSL\wsl\DockerDesktopWSL)", default_docker}));
  EXPECT_EQ(tool_variable(L"docker_wsl_data", owner(),
                          files_of(std::nullopt, {{settings_store, R"({"AutoStart": false})"}})),
            std::vector<std::wstring>{default_docker});
}

// Which folder Docker uses is unknown when no settings file exists, or one that exists cannot be
// read or parsed or names a relative folder: then nothing may be offered, so there is no value.
TEST_F(ToolVariable, DockerWslDataHasNoValueWhenTheSettingsCannotBeRead) {
  const std::string live = R"({"CustomWslDistroDir": "D:\\Database\\DockerDesktopWSL"})";
  for (const ToolFiles& files :
       {files_of(std::nullopt, {}),
        files_of(std::nullopt,
                 {{settings_store, live}, {legacy_settings, fail(Errc::access_denied, "denied")}}),
        files_of(std::nullopt, {{settings_store, fail(Errc::refused, "a link")}}),
        files_of(std::nullopt, {{settings_store, R"({"CustomWslDistroDir": )"}}),
        files_of(std::nullopt, {{settings_store, R"({"CustomWslDistroDir": 3})"}}),
        files_of(std::nullopt,
                 {{settings_store, R"({"CustomWslDistroDir": "DockerDesktopWSL"})"}})}) {
    EXPECT_EQ(tool_variable(L"DOCKER_WSL_DATA", owner(), files), std::vector<std::wstring>{});
  }
  EXPECT_EQ(tool_variable(L"DOCKER_WSL_DATA", FakeEnvironment{},
                          files_of(std::nullopt, {{settings_store, live}})),
            std::vector<std::wstring>{});
}

TEST_F(ToolVariable, OtherVariablesAreNotToolVariables) {
  EXPECT_EQ(tool_variable(L"TEMP", owner(), files_of(steam, {})), std::nullopt);
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY_X", owner(), files_of(steam, {})), std::nullopt);
  EXPECT_TRUE(is_tool_variable(L"Steam_Library"));
  EXPECT_TRUE(is_tool_variable(L"DOCKER_WSL_DATA"));
  EXPECT_FALSE(is_tool_variable(L"TEMP"));
}

}  // namespace
}  // namespace myclean::clean
