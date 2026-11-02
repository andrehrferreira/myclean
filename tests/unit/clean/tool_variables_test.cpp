#include "core/clean/tool_variables.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::clean {
namespace {

namespace fs = std::filesystem;

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

// The reference machine's libraryfolders.vdf: Steam's own folder and libraries on other drives,
// one written with a trailing separator and '/'.
TEST(ToolVariable, SteamLibraryIsEveryLibraryOfLibraryFolders) {
  const ToolFiles files = files_of(steam, {{vdf, R"("libraryfolders"
{
  "0" { "path" "C:\\Program Files (x86)\\Steam" "label" "" }
  "1" { "path" "E:\\SteamLibrary" "apps" { "275850" "33478918624" } }
  "2" { "path" "D:/Games/Steam Library/" }
})"}});

  EXPECT_EQ(tool_variable(L"steam_library", files),
            (std::vector<std::wstring>{LR"(C:\Program Files (x86)\Steam)", LR"(E:\SteamLibrary)",
                                       LR"(D:\Games\Steam Library)"}));
}

// Without Steam, or when its file cannot be read or parsed, the variable has no value: a target
// naming it selects nothing.
TEST(ToolVariable, SteamLibraryHasNoValueWithoutAReadableFile) {
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", files_of(std::nullopt, {})),
            std::vector<std::wstring>{});
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", files_of(steam, {})), std::vector<std::wstring>{});
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY",
                          files_of(steam, {{vdf, fail(Errc::access_denied, "denied")}})),
            std::vector<std::wstring>{});
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY", files_of(steam, {{vdf, R"("libraryfolders" {)"}})),
            std::vector<std::wstring>{});
}

TEST(ToolVariable, OtherVariablesAreNotToolVariables) {
  EXPECT_EQ(tool_variable(L"TEMP", files_of(steam, {})), std::nullopt);
  EXPECT_EQ(tool_variable(L"STEAM_LIBRARY_X", files_of(steam, {})), std::nullopt);
  EXPECT_TRUE(is_tool_variable(L"Steam_Library"));
  EXPECT_FALSE(is_tool_variable(L"TEMP"));
}

}  // namespace
}  // namespace myclean::clean
