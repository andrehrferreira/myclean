#include "core/indexer/scope.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace myclean::indexer {
namespace {

wchar_t fold(wchar_t c) {
  return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool same_url(const std::wstring& left, const std::wstring& right) {
  return std::ranges::equal(left, right, [](wchar_t a, wchar_t b) { return fold(a) == fold(b); });
}

}  // namespace

std::wstring folder_url(const std::filesystem::path& folder) {
  std::wstring url = L"file:///" + folder.native();
  if (url.back() != L'\\') {
    url.push_back(L'\\');
  }
  return url;
}

const ScopeRule* user_rule(const std::vector<ScopeRule>& rules, const std::wstring& url) {
  const auto found = std::ranges::find_if(
      rules, [&](const ScopeRule& rule) { return rule.user && same_url(rule.url, url); });
  return found == rules.end() ? nullptr : &*found;
}

}  // namespace myclean::indexer
