#include "core/clean/patterns.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/clean/probes.hpp"

namespace myclean::clean {

namespace {

wchar_t fold(wchar_t character) {
  return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a')
                                                : character;
}

std::wstring folded(std::wstring_view text) {
  std::wstring result(text);
  for (wchar_t& character : result) {
    character = fold(character);
  }
  return result;
}

}  // namespace

std::optional<std::wstring> expand_variables(std::wstring_view text,
                                             const IEnvironment& environment) {
  std::wstring result;
  std::size_t position = 0;
  while (position < text.size()) {
    const std::size_t open = text.find(L'%', position);
    const std::size_t close =
        open == std::wstring_view::npos ? std::wstring_view::npos : text.find(L'%', open + 1);
    if (close == std::wstring_view::npos) {
      result.append(text.substr(position));
      break;
    }
    result.append(text.substr(position, open - position));
    const std::optional<std::wstring> value =
        environment.variable(text.substr(open + 1, close - open - 1));
    if (!value) {
      return std::nullopt;
    }
    result.append(*value);
    position = close + 1;
  }
  return result;
}

bool has_wildcard(std::wstring_view pattern) {
  return pattern.find_first_of(L"*?") != std::wstring_view::npos;
}

bool wildcard_match(std::wstring_view name, std::wstring_view pattern) {
  // Greedy matching with backtracking to the last '*': linear for the patterns rules use.
  std::size_t at_name = 0;
  std::size_t at_pattern = 0;
  std::optional<std::size_t> star;
  std::size_t star_name = 0;
  while (at_name < name.size()) {
    if (at_pattern < pattern.size() &&
        (pattern[at_pattern] == L'?' || fold(pattern[at_pattern]) == fold(name[at_name]))) {
      ++at_name;
      ++at_pattern;
    } else if (at_pattern < pattern.size() && pattern[at_pattern] == L'*') {
      star = at_pattern++;
      star_name = at_name;
    } else if (star) {
      at_pattern = *star + 1;
      at_name = ++star_name;
    } else {
      return false;
    }
  }
  while (at_pattern < pattern.size() && pattern[at_pattern] == L'*') {
    ++at_pattern;
  }
  return at_pattern == pattern.size();
}

bool within(const std::filesystem::path& path, const std::filesystem::path& base) {
  const std::wstring one = folded(path.native());
  const std::wstring other = folded(base.native());
  if (other.empty() || !one.starts_with(other)) {
    return false;
  }
  return one.size() == other.size() || other.ends_with(L'\\') || one[other.size()] == L'\\';
}

}  // namespace myclean::clean
