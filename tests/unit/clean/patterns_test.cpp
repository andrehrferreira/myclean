#include "core/clean/patterns.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "clean_fakes.hpp"
#include "core/clean/walk.hpp"
#include "core/model/units.hpp"

namespace myclean::clean {
namespace {

using namespace myclean::literals;
using testing::FakeEnvironment;
using testing::FakeTree;
namespace fs = std::filesystem;

TEST(ExpandVariables, ReplacesEveryVariableCaseInsensitively) {
  FakeEnvironment environment;
  environment.set(L"LOCALAPPDATA", LR"(C:\Users\owner\AppData\Local)");
  environment.set(L"ProgramFiles(x86)", LR"(C:\Program Files (x86))");
  EXPECT_EQ(expand_variables(LR"(%localappdata%\NVIDIA\DXCache\*)", environment),
            LR"(C:\Users\owner\AppData\Local\NVIDIA\DXCache\*)");
  EXPECT_EQ(expand_variables(LR"(%ProgramFiles(x86)%\Steam)", environment),
            LR"(C:\Program Files (x86)\Steam)");
}

TEST(ExpandVariables, UnsetVariableMeansNoTarget) {
  const FakeEnvironment environment;
  EXPECT_EQ(expand_variables(LR"(%GRADLE_USER_HOME%\caches)", environment), std::nullopt);
}

TEST(ExpandVariables, LonePercentIsLiteral) {
  const FakeEnvironment environment;
  EXPECT_EQ(expand_variables(LR"(C:\100% done)", environment), LR"(C:\100% done)");
}

TEST(WildcardMatch, StarAndQuestionMarkCaseInsensitive) {
  EXPECT_TRUE(wildcard_match(L"Cache", L"cache"));
  EXPECT_TRUE(wildcard_match(L"webcache_4430", L"webcache*"));
  EXPECT_TRUE(wildcard_match(L"IntelliJIdea2023.1", L"*20??.*"));
  EXPECT_FALSE(wildcard_match(L"Toolbox", L"*20??.*"));
  EXPECT_TRUE(wildcard_match(L"9.3.1", L"?.*"));
  EXPECT_FALSE(wildcard_match(L"modules-2", L"?.*"));
  EXPECT_TRUE(wildcard_match(L"requirements-dev.txt", L"requirements*.txt"));
  EXPECT_FALSE(wildcard_match(L"Cache_Data", L"Cache"));
  EXPECT_TRUE(wildcard_match(L"", L"*"));
  EXPECT_FALSE(wildcard_match(L"", L"?"));
}

TEST(Within, ComparesWholeComponents) {
  EXPECT_TRUE(within(R"(C:\a\b)", R"(c:\A)"));
  EXPECT_TRUE(within(R"(C:\a)", R"(C:\a)"));
  EXPECT_TRUE(within(R"(C:\a)", R"(C:\)"));
  EXPECT_FALSE(within(R"(C:\ab)", R"(C:\a)"));
  EXPECT_FALSE(within(R"(C:\a)", R"(C:\a\b)"));
}

TEST(Resolve, WildcardComponentsMatchFoldersOnlyBeforeTheLast) {
  FakeTree tree;
  tree.add_file(R"(C:\U\Default\Cache\data_0)", 1_KiB, {});
  tree.add_file(R"(C:\U\Profile 1\Cache\data_0)", 1_KiB, {});
  tree.add_file(R"(C:\U\Local State)", 1_KiB, {});
  tree.add_directory(R"(C:\U\Default\Network)");

  const Matches matches = resolve(tree, fs::path{R"(C:\U\*\Cache)"});
  ASSERT_EQ(matches.entries.size(), 2U);
  EXPECT_EQ(matches.entries[0].path, fs::path{R"(C:\U\Default\Cache)"});
  EXPECT_EQ(matches.entries[1].path, fs::path{R"(C:\U\Profile 1\Cache)"});
  EXPECT_TRUE(matches.links.empty());
}

TEST(Resolve, NeverPassesThroughALink) {
  FakeTree tree;
  tree.add_directory(R"(C:\U\Default)", {}, {.reparse_point = true});
  tree.add_file(R"(C:\U\Real\Cache\x)", 1_KiB, {});

  const Matches matches = resolve(tree, fs::path{R"(C:\U\*\Cache)"});
  ASSERT_EQ(matches.entries.size(), 1U);
  EXPECT_EQ(matches.entries[0].path, fs::path{R"(C:\U\Real\Cache)"});
  EXPECT_EQ(matches.links, (std::vector<fs::path>{R"(C:\U\Default)"}));
  EXPECT_EQ(tree.listed(R"(C:\U\Default)"), 0);
}

TEST(Resolve, DotDotMatchesNothing) {
  FakeTree tree;
  tree.add_file(R"(C:\secret)", 1_KiB, {});
  tree.add_directory(R"(C:\project)");
  EXPECT_TRUE(resolve(tree, R"(C:\project)", R"(..\secret)").entries.empty());
}

TEST(Measure, SumsSizeOnDiskAndNewestTimeWithoutEnteringLinks) {
  FakeTree tree;
  const WallTime old_time = WallTime{} + std::chrono::hours{10};
  const WallTime new_time = WallTime{} + std::chrono::hours{20};
  tree.add_directory(R"(C:\c)", old_time);
  tree.add_file(R"(C:\c\a)", 3_KiB, old_time);
  tree.add_file(R"(C:\c\sub\b)", 5_KiB, new_time);
  tree.add_directory(R"(C:\c\link)", old_time, {.reparse_point = true});

  const Measure measured = measure(tree, *tree.stat(R"(C:\c)"));
  EXPECT_EQ(measured.size, 8_KiB);
  EXPECT_EQ(measured.newest, new_time);
  EXPECT_TRUE(measured.complete);
  EXPECT_EQ(tree.listed(R"(C:\c\link)"), 0);
}

TEST(Measure, UnreadableFolderMakesItIncomplete) {
  FakeTree tree;
  tree.add_file(R"(C:\c\sub\b)", 5_KiB, {});
  tree.deny(R"(C:\c\sub)");
  EXPECT_FALSE(measure(tree, *tree.stat(R"(C:\c)")).complete);
}

}  // namespace
}  // namespace myclean::clean
