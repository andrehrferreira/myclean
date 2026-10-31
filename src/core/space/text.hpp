#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Small text helpers shared by the space analyses.
namespace myclean::space {

// UTF-8 text of a path.
[[nodiscard]] inline std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// `text` with ASCII letters lower-cased; other bytes unchanged.
[[nodiscard]] inline std::string lower_ascii(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lower;
}

// The extension of `path` in lower case, with its dot (".msi"); empty when there is none.
[[nodiscard]] inline std::string lower_extension(const std::filesystem::path& path) {
  return lower_ascii(utf8(path.extension()));
}

}  // namespace myclean::space
