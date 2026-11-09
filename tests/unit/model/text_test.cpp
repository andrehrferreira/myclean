#include "core/model/text.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace myclean {
namespace {

TEST(Utf8TextTest, ConvertsValidNames) {
  EXPECT_EQ(utf8_text(std::filesystem::path(L"C:\\Users\\o\\Downloads\\a.txt")),
            "C:\\Users\\o\\Downloads\\a.txt");
  // é (2 bytes), € (3 bytes), 😀 (a surrogate pair, 4 bytes).
  EXPECT_EQ(utf8_text(std::filesystem::path(L"caf\u00e9 \u20ac \U0001F600")),
            "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
  EXPECT_EQ(utf8_text(std::filesystem::path()), "");
}

// Found on F:'s Recycle Bin: ".\udc6d\udc73\udc79\udc73000b..." (lone low surrogates). u8string()
// throws on it, which aborted scan-all.
TEST(Utf8TextTest, LoneSurrogatesBecomeReplacementCharacters) {
  const std::wstring name{L'.', static_cast<wchar_t>(0xDC6D), static_cast<wchar_t>(0xDC73), L'0'};
  EXPECT_EQ(utf8_text(std::filesystem::path(name)),
            ".\xEF\xBF\xBD\xEF\xBF\xBD"
            "0");
  const std::wstring high_at_end{L'a', static_cast<wchar_t>(0xD83D)};
  EXPECT_EQ(utf8_text(std::filesystem::path(high_at_end)), "a\xEF\xBF\xBD");
  const std::wstring high_then_letter{static_cast<wchar_t>(0xD83D), L'b'};
  EXPECT_EQ(utf8_text(std::filesystem::path(high_then_letter)),
            "\xEF\xBF\xBD"
            "b");
}

}  // namespace
}  // namespace myclean
