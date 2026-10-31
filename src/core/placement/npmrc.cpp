#include "core/placement/npmrc.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace myclean::placement {

namespace {

constexpr std::string_view blanks = " \t\r";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// The parts of `text` between line feeds; joining them with "\n" gives `text` back.
std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t end = text.find('\n'); end != std::string_view::npos;
       end = text.find('\n', start)) {
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  lines.emplace_back(text.substr(start));
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string text;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (index > 0) {
      text += '\n';
    }
    text += lines[index];
  }
  return text;
}

// The value `line` assigns to `key`; nullopt when it assigns another key or none.
std::optional<std::string_view> assignment(std::string_view line, std::string_view key) {
  const std::string_view content = trim(line);
  if (content.empty() || content.front() == ';' || content.front() == '#') {
    return std::nullopt;
  }
  const std::size_t equals = content.find('=');
  if (equals == std::string_view::npos || trim(content.substr(0, equals)) != key) {
    return std::nullopt;
  }
  return trim(content.substr(equals + 1));
}

}  // namespace

std::optional<std::string> npmrc_value(std::string_view text, std::string_view key) {
  std::optional<std::string> value;
  for (const std::string& line : split_lines(text)) {
    if (const std::optional<std::string_view> assigned = assignment(line, key)) {
      std::string_view found = *assigned;
      if (found.size() >= 2 && (found.front() == '"' || found.front() == '\'') &&
          found.back() == found.front()) {
        found = found.substr(1, found.size() - 2);
      }
      value = std::string(found);
    }
  }
  return value;
}

std::string npmrc_set(std::string_view text, std::string_view key, std::string_view value) {
  std::vector<std::string> lines = split_lines(text);
  const bool crlf = text.find("\r\n") != std::string_view::npos;
  std::string line = std::string(key) + "=" + std::string(value) + (crlf ? "\r" : "");
  for (std::size_t index = lines.size(); index-- > 0;) {
    if (assignment(lines[index], key)) {
      lines[index] = std::move(line);
      return join_lines(lines);
    }
  }
  if (lines.back().empty()) {
    lines.insert(lines.end() - 1, std::move(line));
  } else {
    lines.push_back(std::move(line));
  }
  return join_lines(lines);
}

std::string npmrc_remove(std::string_view text, std::string_view key) {
  std::vector<std::string> lines = split_lines(text);
  std::erase_if(lines, [&](const std::string& line) { return assignment(line, key).has_value(); });
  return join_lines(lines);
}

}  // namespace myclean::placement
