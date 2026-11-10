#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/indexer/scope.hpp"
#include "core/model/result.hpp"

namespace myclean::indexer::testing {

// A crawl scope in memory. A folder is crawled as the rule with the longest URL that holds it
// says (a user rule before a default rule of the same URL); no rule: not crawled. Rule URLs are
// folder URLs ("file:///F:\"), compared case-insensitively.
class FakeScope final : public IIndexerScope {
 public:
  std::vector<ScopeRule> rules_list;
  std::optional<Error> failure;  // every call fails with it
  int writes = 0;                // set_rule and remove_rule calls that changed the scope

  [[nodiscard]] Result<std::vector<ScopeRule>> rules() const override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    return rules_list;
  }

  [[nodiscard]] Result<bool> included(const std::filesystem::path& folder) const override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    const std::wstring url = lower(folder_url(folder));
    const ScopeRule* best = nullptr;
    for (const ScopeRule& rule : rules_list) {
      const std::wstring prefix = lower(rule.url);
      if (!url.starts_with(prefix)) {
        continue;
      }
      if (best == nullptr || prefix.size() > best->url.size() ||
          (prefix.size() == best->url.size() && rule.user)) {
        best = &rule;
      }
    }
    return best != nullptr && best->included;
  }

  [[nodiscard]] Result<void> set_rule(const std::filesystem::path& folder, bool include) override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    const std::wstring url = folder_url(folder);
    std::erase_if(rules_list, [&](const ScopeRule& rule) {
      return rule.user && lower(rule.url) == lower(url);
    });
    rules_list.push_back(ScopeRule{.url = url, .included = include, .user = true});
    ++writes;
    return {};
  }

  [[nodiscard]] Result<void> remove_rule(const std::filesystem::path& folder) override {
    if (failure) {
      return tl::unexpected(*failure);
    }
    const std::wstring url = folder_url(folder);
    const std::size_t removed = std::erase_if(rules_list, [&](const ScopeRule& rule) {
      return rule.user && lower(rule.url) == lower(url);
    });
    if (removed == 0) {
      return fail(Errc::not_found, "no index rule");
    }
    ++writes;
    return {};
  }

 private:
  static std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return text;
  }
};

}  // namespace myclean::indexer::testing
