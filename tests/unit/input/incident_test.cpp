#include "core/input/incident.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace myclean::input {
namespace {

TEST(Chord, TextFormsRoundTrip) {
  EXPECT_EQ(to_string(Chord::copy), "copy");
  EXPECT_EQ(to_string(Chord::paste), "paste");
  EXPECT_EQ(parse_chord("paste"), Chord::paste);
  EXPECT_EQ(parse_chord("cut"), std::nullopt);
}

TEST(DescribeFormats, NamesStandardFormatsAndMarksSynthesizedText) {
  // As observed in spike X2 for a VS Code text copy.
  const std::vector<ClipboardFormat> described =
      describe_formats({{.id = 13}, {.id = 16}, {.id = 1}, {.id = 7}});
  const std::vector<ClipboardFormat> expected = {
      {.id = 13, .name = "CF_UNICODETEXT", .synthesized = false},
      {.id = 16, .name = "CF_LOCALE", .synthesized = false},
      {.id = 1, .name = "CF_TEXT", .synthesized = true},
      {.id = 7, .name = "CF_OEMTEXT", .synthesized = true}};
  EXPECT_EQ(described, expected);
}

TEST(DescribeFormats, FirstOfEachGroupIsOriginal) {
  const std::vector<ClipboardFormat> described =
      describe_formats({{.id = 8}, {.id = 14}, {.id = 17}, {.id = 2}, {.id = 3}, {.id = 1}});
  EXPECT_FALSE(described[0].synthesized);  // CF_DIB
  EXPECT_FALSE(described[1].synthesized);  // CF_ENHMETAFILE
  EXPECT_TRUE(described[2].synthesized);   // CF_DIBV5
  EXPECT_TRUE(described[3].synthesized);   // CF_BITMAP
  EXPECT_TRUE(described[4].synthesized);   // CF_METAFILEPICT
  EXPECT_FALSE(described[5].synthesized);  // CF_TEXT, a different group
}

TEST(DescribeFormats, RegisteredNamesAreKeptAndUnknownIdsStayUnnamed) {
  const std::vector<ClipboardFormat> described =
      describe_formats({{.id = 0xC123, .name = "HTML Format"}, {.id = 0x200}});
  EXPECT_EQ(described[0].name, "HTML Format");
  EXPECT_FALSE(described[0].synthesized);
  EXPECT_EQ(described[1].name, "");
}

TEST(IsHeavy, ImagesAndRichFormatsAreHeavy) {
  for (const unsigned id : {2U, 3U, 6U, 8U, 14U, 17U}) {
    EXPECT_TRUE(is_heavy({.id = id})) << id;
  }
  for (const std::string_view name :
       {"PNG", "png", "JFIF", "GIF", "HTML Format", "Rich Text Format"}) {
    EXPECT_TRUE(is_heavy({.id = 0xC100, .name = std::string(name)})) << name;
  }
  for (const unsigned id : {1U, 7U, 13U, 15U, 16U}) {
    EXPECT_FALSE(is_heavy({.id = id})) << id;
  }
  EXPECT_FALSE(is_heavy({.id = 0xC100, .name = "Shell IDList Array"}));
}

TEST(RunningSuspects, MatchesWithoutCaseInSuspectOrder) {
  const std::vector<std::string> running = {"explorer.exe", "discord.exe", "Code.exe",
                                            "POWERTOYS.KEYBOARDMANAGERENGINE.EXE", "Discord.exe"};
  const std::vector<std::string> expected = {"PowerToys.KeyboardManagerEngine.exe", "Discord.exe"};
  EXPECT_EQ(running_suspects(running), expected);
  EXPECT_TRUE(running_suspects(std::vector<std::string>{"notepad.exe"}).empty());
  EXPECT_FALSE(known_suspects().empty());
}

TEST(CpuPercent, IsBusyShareOfElapsedTime) {
  EXPECT_EQ(cpu_percent({.idle = 100, .busy = 100}, {.idle = 200, .busy = 400}), 75.0);
  EXPECT_EQ(cpu_percent({.idle = 0, .busy = 0}, {.idle = 0, .busy = 50}), 100.0);
  EXPECT_EQ(cpu_percent({.idle = 5, .busy = 5}, {.idle = 5, .busy = 5}), std::nullopt);
  EXPECT_EQ(cpu_percent({.idle = 9, .busy = 5}, {.idle = 5, .busy = 9}), std::nullopt);
}

}  // namespace
}  // namespace myclean::input
