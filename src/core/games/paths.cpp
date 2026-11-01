#include "core/games/paths.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace myclean::games {

namespace {

wchar_t fold(wchar_t c) {
  return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

}  // namespace

std::wstring path_key(const std::filesystem::path& path) {
  std::wstring key = path.lexically_normal().make_preferred().native();
  constexpr std::wstring_view unc_prefix = L"\\\\?\\UNC\\";
  constexpr std::wstring_view long_prefix = L"\\\\?\\";
  if (key.starts_with(unc_prefix)) {
    key.replace(0, unc_prefix.size(), L"\\\\");
  } else if (key.starts_with(long_prefix)) {
    key.erase(0, long_prefix.size());
  }
  std::ranges::transform(key, key.begin(), fold);
  while (key.size() > 1 && key.back() == L'\\' && key[key.size() - 2] != L':') {
    key.pop_back();
  }
  return key;
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
  return path_key(left) == path_key(right);
}

bool path_within(const std::filesystem::path& path, const std::filesystem::path& root) {
  const std::wstring base = path_key(root);
  const std::wstring key = path_key(path);
  if (base.empty() || !key.starts_with(base)) {
    return false;
  }
  return key.size() == base.size() || base.back() == L'\\' || key[base.size()] == L'\\';
}

bool in_windows_apps(const std::filesystem::path& path) {
  constexpr std::wstring_view windows_apps = L"windowsapps";
  return std::ranges::any_of(path, [windows_apps](const std::filesystem::path& part) {
    return std::ranges::equal(part.native(), windows_apps, {}, fold);
  });
}

std::filesystem::path path_from_utf8(std::string_view text) {
  return std::filesystem::path{std::u8string(text.begin(), text.end())};
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

}  // namespace myclean::games
