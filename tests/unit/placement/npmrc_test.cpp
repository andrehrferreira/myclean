#include "core/placement/npmrc.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace myclean::placement {
namespace {

TEST(Npmrc, ReadsTheLastAssignmentAndSkipsComments) {
  const std::string text =
      "; cache=C:\\old\r\ncache = \"D:\\first\"\r\nregistry=x\r\ncache=E:\\last\r\n";
  EXPECT_EQ(npmrc_value(text, "cache"), std::optional<std::string>{"E:\\last"});
  EXPECT_EQ(npmrc_value(text, "registry"), std::optional<std::string>{"x"});
  EXPECT_EQ(npmrc_value("# cache=x\n", "cache"), std::nullopt);
  EXPECT_EQ(npmrc_value("", "cache"), std::nullopt);
}

TEST(Npmrc, SetReplacesTheLastAssignmentAndKeepsEverythingElse) {
  EXPECT_EQ(npmrc_set("a=1\ncache=old\nb=2\n", "cache", "D:\\npm"), "a=1\ncache=D:\\npm\nb=2\n");
  EXPECT_EQ(npmrc_set("a=1\r\n", "cache", "D:\\npm"), "a=1\r\ncache=D:\\npm\r\n");
  EXPECT_EQ(npmrc_set("a=1", "cache", "D:\\npm"), "a=1\ncache=D:\\npm");
  EXPECT_EQ(npmrc_set("", "cache", "D:\\npm"), "cache=D:\\npm\n");
}

TEST(Npmrc, RemoveDropsEveryAssignmentOfTheKeyOnly) {
  EXPECT_EQ(npmrc_remove("a=1\ncache=x\n; note\ncache=y\n", "cache"), "a=1\n; note\n");
  EXPECT_EQ(npmrc_remove("cache=D:\\npm\n", "cache"), "");
  EXPECT_EQ(npmrc_remove("a=1\n", "cache"), "a=1\n");
}

}  // namespace
}  // namespace myclean::placement
