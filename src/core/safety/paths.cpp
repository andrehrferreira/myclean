#include "core/safety/paths.hpp"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"

namespace myclean::safety {

namespace {

bool is_ascii_letter(wchar_t character) {
  return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
}

bool is_valid_component(std::wstring_view component) {
  return !component.empty() && component.find_first_of(L"<>:\"|?*") == std::wstring_view::npos &&
         std::ranges::none_of(component, [](wchar_t character) { return character < L' '; });
}

}  // namespace

std::string to_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Result<std::filesystem::path> normalize_path(const std::filesystem::path& path) {
  std::wstring text = path.native();
  std::ranges::replace(text, L'/', L'\\');
  for (const std::wstring_view prefix :
       {std::wstring_view{L"\\\\?\\"}, std::wstring_view{L"\\??\\"}}) {
    if (text.starts_with(prefix)) {
      text.erase(0, prefix.size());
      break;
    }
  }
  if (text.size() < 3 || !is_ascii_letter(text[0]) || text[1] != L':' || text[2] != L'\\') {
    return fail(Errc::invalid_argument, "not an absolute drive path: " + to_utf8(path));
  }

  std::vector<std::wstring> components;
  for (const auto piece : std::wstring_view{text}.substr(3) | std::views::split(L'\\')) {
    std::wstring component(piece.begin(), piece.end());
    if (component.empty() || component == L".") {
      continue;
    }
    if (component == L"..") {
      if (!components.empty()) {
        components.pop_back();
      }
      continue;
    }
    while (!component.empty() && (component.back() == L'.' || component.back() == L' ')) {
      component.pop_back();
    }
    if (!is_valid_component(component)) {
      return fail(Errc::invalid_argument, "invalid path component in: " + to_utf8(path));
    }
    components.push_back(std::move(component));
  }

  const wchar_t drive = text[0] >= L'a' ? static_cast<wchar_t>(text[0] - L'a' + L'A') : text[0];
  std::wstring normal{drive, L':', L'\\'};
  for (const std::wstring& component : components) {
    if (normal.size() > 3) {
      normal += L'\\';
    }
    normal += component;
  }
  return std::filesystem::path{normal};
}

}  // namespace myclean::safety
