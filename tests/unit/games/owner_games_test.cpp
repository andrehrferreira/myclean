#include "core/games/owner_games.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::games {
namespace {

namespace fs = std::filesystem;

TEST(OwnerGames, ReadsTheOwnersListAndLibraryRoots) {
  const Result<OwnerGames> games = parse_owner_games(R"toml(
owner = ["Diablo IV.exe", 'D:\Tools\Emulator\emu.exe']
roots = ['D:\Games', 'E:\Itch']
)toml");
  ASSERT_TRUE(games.has_value()) << games.error().message;
  EXPECT_EQ(games->owner,
            (std::vector<std::string>{"Diablo IV.exe", R"(D:\Tools\Emulator\emu.exe)"}));
  EXPECT_EQ(games->roots, (std::vector<fs::path>{LR"(D:\Games)", LR"(E:\Itch)"}));
}

TEST(OwnerGames, BothKeysAreOptional) {
  const Result<OwnerGames> games = parse_owner_games("# nothing yet\n");
  ASSERT_TRUE(games.has_value()) << games.error().message;
  EXPECT_TRUE(games->owner.empty());
  EXPECT_TRUE(games->roots.empty());
}

TEST(OwnerGames, RefusesUnknownKeysBadValuesAndRelativeRoots) {
  for (const char* text : {"games = []", "owner = \"x.exe\"", "owner = [\"\"]", "roots = [1]",
                           R"(roots = ['Games'])", R"(roots = ['\\server\share'])", "owner = ["}) {
    const Result<OwnerGames> games = parse_owner_games(text);
    ASSERT_FALSE(games.has_value()) << text;
    EXPECT_EQ(games.error().code, Errc::invalid_data) << text;
    EXPECT_TRUE(games.error().message.starts_with("games.toml")) << games.error().message;
  }
}

}  // namespace
}  // namespace myclean::games
