#include "core/clean/workspaces.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"

namespace myclean::clean {
namespace {

const std::filesystem::path file = R"(C:\MyClean\data\workspaces.toml)";

std::string rejection(std::string_view text) {
  const Result<Workspaces> parsed = parse_workspace_file(text, file);
  EXPECT_FALSE(parsed.has_value());
  return parsed ? std::string{} : parsed.error().message;
}

TEST(ParseWorkspaces, ReadsRootsSkipAndDepth) {
  const Result<Workspaces> parsed = parse_workspace_file(
      "roots = ['F:\\', 'W:\\code']\nskip = ['node_modules']\nmax_depth = 5\n", file);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->roots, (std::vector<std::filesystem::path>{R"(F:\)", R"(W:\code)"}));
  EXPECT_EQ(parsed->skip, (std::vector<std::wstring>{L"node_modules"}));
  EXPECT_EQ(parsed->max_depth, 5);
}

TEST(ParseWorkspaces, DefaultsWithoutOptionalFields) {
  const Result<Workspaces> parsed = parse_workspace_file("roots = ['F:\\']\n", file);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_TRUE(parsed->skip.empty());
  EXPECT_EQ(parsed->max_depth, 8);
}

TEST(ParseWorkspaces, RejectsInvalidFilesNamingTheField) {
  EXPECT_EQ(rejection("skip = ['x']\n"),
            R"(C:\MyClean\data\workspaces.toml: field 'roots' is missing)");
  EXPECT_NE(rejection("roots = ['relative\\path']\n").find("field 'roots' must hold absolute"),
            std::string::npos);
  EXPECT_NE(rejection("roots = []\n").find("field 'roots' must be a non-empty list"),
            std::string::npos);
  EXPECT_NE(rejection("roots = ['F:\\']\nmax_depth = 0\n").find("field 'max_depth'"),
            std::string::npos);
  EXPECT_NE(rejection("roots = ['F:\\']\ndepth = 3\n").find("field 'depth' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("roots = [").find("is not valid TOML"), std::string::npos);
}

}  // namespace
}  // namespace myclean::clean
