// Win32IndexerScope against this machine, read only: the crawl scope is listed and queried, never
// changed (set_rule and remove_rule are not called here; the action's undo is tested over a fake
// in tests/unit/indexer). Skipped when Windows Search is not running.
#include "platform/indexer/win32_indexer_scope.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/indexer/scope.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

TEST(Win32IndexerScopeTest, ReadsTheCrawlScopeOfThisMachine) {
  const Win32IndexerScope scope;
  const Result<std::vector<indexer::ScopeRule>> rules = scope.rules();
  if (!rules) {
    GTEST_SKIP() << "Windows Search is not available: " << rules.error().message;
  }
  // Windows always has default rules (the Start menu and the users' folders, with AppData out).
  EXPECT_TRUE(std::ranges::any_of(
      *rules, [](const indexer::ScopeRule& rule) { return !rule.user && !rule.url.empty(); }));

  // The Windows folder is never in the default scope.
  const Result<bool> windows = scope.included(LR"(C:\Windows\System32)");
  ASSERT_TRUE(windows.has_value()) << windows.error().message;
  EXPECT_FALSE(*windows);

  // A folder named by an included rule without wildcards is crawled.
  constexpr std::wstring_view prefix = L"file:///";
  const auto plain = std::ranges::find_if(*rules, [&](const indexer::ScopeRule& rule) {
    return rule.included && rule.url.starts_with(prefix) &&
           rule.url.find_first_of(L"*?") == std::wstring::npos && rule.url.size() > prefix.size();
  });
  if (plain != rules->end()) {
    const std::filesystem::path folder{plain->url.substr(prefix.size())};
    const Result<bool> crawled = scope.included(folder);
    ASSERT_TRUE(crawled.has_value()) << crawled.error().message;
    EXPECT_TRUE(*crawled);
  }
}

}  // namespace
}  // namespace myclean::platform
