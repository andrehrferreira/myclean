#include "core/games/steam.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::games {
namespace {

namespace fs = std::filesystem;

// The current format, as Steam writes it (spike X5 §2): stale entries included.
constexpr std::string_view current_vdf = R"vdf("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"1234567890123456789"
		"totalsize"		"0"
		"update_clean_bytes_tally"		"0"
		"time_last_update_verified"		"0"
		"apps"
		{
			"228980"		"512000"
		}
	}
	"1"
	{
		"path"		"E:\\SteamLibrary"
		"label"		"Tools & Games"
		"apps"
		{
			"1623730"		"25000000000"
			"413150"		"1000000000"
		}
	}
	"2"
	{
		"path"		"L:\\SteamLibrary"
		"apps"
		{
		}
	}
}
)vdf";

TEST(LibraryFolders, ReadsEveryPathOfTheCurrentFormat) {
  const Result<std::vector<fs::path>> folders = parse_library_folders(current_vdf);
  ASSERT_TRUE(folders) << folders.error().message;
  const std::vector<fs::path> expected = {fs::path{LR"(C:\Program Files (x86)\Steam)"},
                                          fs::path{LR"(E:\SteamLibrary)"},
                                          fs::path{LR"(L:\SteamLibrary)"}};
  EXPECT_EQ(*folders, expected);
}

TEST(LibraryFolders, ReadsTheOldFormatAndSkipsItsOtherKeys) {
  const Result<std::vector<fs::path>> folders = parse_library_folders(R"vdf(
// written by an old Steam client
"LibraryFolders"
{
	"TimeNextStatsReport"		"1700000000"
	"ContentStatsID"		"-123"
	"1"		"D:\\Games\\Steam"
	"2"		"F:/SteamLibrary"
}
)vdf");
  ASSERT_TRUE(folders) << folders.error().message;
  const std::vector<fs::path> expected = {fs::path{LR"(D:\Games\Steam)"},
                                          fs::path{LR"(F:\SteamLibrary)"}};
  EXPECT_EQ(*folders, expected);
}

TEST(LibraryFolders, AcceptsBareTokensConditionalsAndUtf8) {
  const Result<std::vector<fs::path>> folders = parse_library_folders(
      "libraryfolders { 0 { path \"E:\\\\Bibliot\xC3\xA9\x63\x61\" [$WIN32] label x } }");
  ASSERT_TRUE(folders) << folders.error().message;
  ASSERT_EQ(folders->size(), 1U);
  EXPECT_EQ(folders->front(), fs::path{L"E:\\Bibliot\u00e9ca"});
}

TEST(LibraryFolders, RejectsMalformedText) {
  const Result<std::vector<fs::path>> unterminated =
      parse_library_folders("\"libraryfolders\"\n{\n\"0\" { \"path\" \"E:\\\\Steam }\n}");
  ASSERT_FALSE(unterminated);
  EXPECT_EQ(unterminated.error().code, Errc::invalid_data);
  EXPECT_EQ(unterminated.error().message, "line 3: unterminated string");

  const Result<std::vector<fs::path>> unclosed =
      parse_library_folders(R"("libraryfolders" { "0" { "path" "E:\\S" })");
  ASSERT_FALSE(unclosed);
  EXPECT_NE(unclosed.error().message.find("missing '}'"), std::string::npos);

  EXPECT_FALSE(parse_library_folders(R"("libraryfolders" { } })"));
  EXPECT_FALSE(parse_library_folders(R"("libraryfolders" { "0" })"));
  const Result<std::vector<fs::path>> other = parse_library_folders(R"("AppState" { })");
  ASSERT_FALSE(other);
  EXPECT_EQ(other.error().message, R"(no "libraryfolders" section)");
}

TEST(LibraryFolders, RejectsDeepNesting) {
  std::string text = R"("libraryfolders")";
  for (int depth = 0; depth < 100; ++depth) {
    text += R"( { "k")";
  }
  EXPECT_FALSE(parse_library_folders(text));
}

TEST(LibraryFolders, FileAndCommonFolder) {
  EXPECT_EQ(library_folders_file(fs::path{"c:/program files (x86)/steam"}),
            fs::path{LR"(c:\program files (x86)\steam\steamapps\libraryfolders.vdf)"});
  EXPECT_EQ(steam_common_folder(fs::path{LR"(E:\SteamLibrary)"}),
            fs::path{LR"(E:\SteamLibrary\steamapps\common)"});
}

}  // namespace
}  // namespace myclean::games
