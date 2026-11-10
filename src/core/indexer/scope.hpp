#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/model/result.hpp"

// The Windows Search index's crawl scope (optimizer spec, "Search-Indexer Scope").
namespace myclean::indexer {

// One crawl-scope rule, as the indexer stores it.
struct ScopeRule {
  std::wstring url;  // "file:///C:\Users\" or a pattern such as "file:///C:\Users\*\AppData\*"
  bool included = false;
  bool user = false;  // a user rule; false: a default rule (Windows' or an application's)

  friend bool operator==(const ScopeRule&, const ScopeRule&) = default;
};

// The URL of the rule that names folder `folder` and everything in it: "file:///" + the path
// with a trailing '\' ("file:///F:\app\node_modules\").
[[nodiscard]] std::wstring folder_url(const std::filesystem::path& folder);

// The user rule of `rules` whose URL is `url`, compared case-insensitively; null when none.
[[nodiscard]] const ScopeRule* user_rule(const std::vector<ScopeRule>& rules,
                                         const std::wstring& url);

// The crawl scope of the SystemIndex catalog (ISearchCrawlScopeManager). Implemented by
// src/platform/indexer; folders are absolute local paths.
class IIndexerScope {
 public:
  virtual ~IIndexerScope() = default;
  IIndexerScope(const IIndexerScope&) = delete;
  IIndexerScope& operator=(const IIndexerScope&) = delete;
  IIndexerScope(IIndexerScope&&) = delete;
  IIndexerScope& operator=(IIndexerScope&&) = delete;

  // Every rule, user and default.
  [[nodiscard]] virtual Result<std::vector<ScopeRule>> rules() const = 0;
  // Whether the indexer crawls `folder`.
  [[nodiscard]] virtual Result<bool> included(const std::filesystem::path& folder) const = 0;
  // Adds the user rule folder_url(`folder`), replacing a user rule of the same URL, and saves
  // the scope. Rules of folders inside it stay as they are.
  [[nodiscard]] virtual Result<void> set_rule(const std::filesystem::path& folder,
                                              bool include) = 0;
  // Removes the user rule folder_url(`folder`) and saves the scope; not_found when there is
  // none.
  [[nodiscard]] virtual Result<void> remove_rule(const std::filesystem::path& folder) = 0;

 protected:
  IIndexerScope() = default;
};

}  // namespace myclean::indexer
