#include "core/games/steam.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/games/paths.hpp"
#include "core/model/result.hpp"

namespace myclean::games {

namespace {

// One key of a KeyValues document: a string value or an object of further keys.
struct KvNode {
  std::string key;
  std::string value;
  std::vector<KvNode> children;
  bool object = false;
};

enum class TokenKind { text, open, close, end };

struct Token {
  TokenKind kind = TokenKind::end;
  std::string text;
};

// Valve KeyValues text: quoted or bare strings, "{" "}", "//" comments and "[$PLATFORM]"
// conditionals, which are skipped.
class KvParser {
 public:
  explicit KvParser(std::string_view text) : text_(text) {}

  Result<std::vector<KvNode>> parse() {
    Result<std::vector<KvNode>> nodes = parse_object(0);
    if (!nodes) {
      return nodes;
    }
    if (current_.kind == TokenKind::close) {
      return error("unexpected '}'");
    }
    return nodes;
  }

 private:
  static constexpr int max_depth = 32;

  // Parses keys until "}" or the end; leaves the closing token in current_.
  // NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by max_depth
  Result<std::vector<KvNode>> parse_object(int depth) {
    if (depth > max_depth) {
      return error("objects nested too deeply");
    }
    std::vector<KvNode> nodes;
    for (;;) {
      if (Result<void> read = next(); !read) {
        return tl::unexpected(read.error());
      }
      if (current_.kind == TokenKind::end || current_.kind == TokenKind::close) {
        return nodes;
      }
      if (current_.kind != TokenKind::text) {
        return error("expected a key");
      }
      KvNode node{.key = std::move(current_.text)};
      if (Result<void> read = next(); !read) {
        return tl::unexpected(read.error());
      }
      if (current_.kind == TokenKind::text) {
        node.value = std::move(current_.text);
      } else if (current_.kind == TokenKind::open) {
        Result<std::vector<KvNode>> children = parse_object(depth + 1);
        if (!children) {
          return children;
        }
        if (current_.kind != TokenKind::close) {
          return error("missing '}'");
        }
        node.object = true;
        node.children = std::move(*children);
      } else {
        return error("expected a value for \"" + node.key + "\"");
      }
      nodes.push_back(std::move(node));
    }
  }

  void skip_space_and_comments() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\n') {
        ++line_;
        ++pos_;
      } else if (c == ' ' || c == '\t' || c == '\r') {
        ++pos_;
      } else if (text_.substr(pos_).starts_with("//")) {
        pos_ = std::min(text_.find('\n', pos_), text_.size());
      } else if (c == '[') {
        // A conditional such as [$WIN32]: platform selection that does not apply to paths.
        pos_ = std::min(text_.find(']', pos_), text_.size());
        if (pos_ < text_.size()) {
          ++pos_;
        }
      } else {
        return;
      }
    }
  }

  Result<void> next() {
    skip_space_and_comments();
    current_ = Token{};
    if (pos_ >= text_.size()) {
      return {};
    }
    const char c = text_[pos_];
    if (c == '{' || c == '}') {
      current_.kind = c == '{' ? TokenKind::open : TokenKind::close;
      ++pos_;
      return {};
    }
    current_.kind = TokenKind::text;
    if (c == '"') {
      return quoted();
    }
    while (pos_ < text_.size()) {
      const char bare = text_[pos_];
      if (bare == ' ' || bare == '\t' || bare == '\r' || bare == '\n' || bare == '{' ||
          bare == '}' || bare == '"') {
        break;
      }
      current_.text.push_back(bare);
      ++pos_;
    }
    return {};
  }

  // A quoted string; \\ \" \n \t are escapes, any other backslash is literal.
  Result<void> quoted() {
    const int start = line_;
    ++pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return {};
      }
      if (c == '\n') {
        ++line_;
      }
      if (c == '\\' && pos_ < text_.size()) {
        const char escaped = text_[pos_];
        if (escaped == '\\' || escaped == '"') {
          current_.text.push_back(escaped);
          ++pos_;
          continue;
        }
        if (escaped == 'n' || escaped == 't') {
          current_.text.push_back(escaped == 'n' ? '\n' : '\t');
          ++pos_;
          continue;
        }
      }
      current_.text.push_back(c);
    }
    line_ = start;
    return error("unterminated string");
  }

  [[nodiscard]] tl::unexpected<Error> error(const std::string& problem) const {
    return fail(Errc::invalid_data, "line " + std::to_string(line_) + ": " + problem);
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  int line_ = 1;
  Token current_;
};

bool equal_folded(std::string_view left, std::string_view right) {
  const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
  return std::ranges::equal(left, right, {}, fold, fold);
}

bool numbered(std::string_view key) {
  return !key.empty() && std::ranges::all_of(key, [](char c) { return c >= '0' && c <= '9'; });
}

const KvNode* child(const std::vector<KvNode>& nodes, std::string_view key) {
  const auto found = std::ranges::find_if(
      nodes, [key](const KvNode& node) { return equal_folded(node.key, key); });
  return found == nodes.end() ? nullptr : &*found;
}

}  // namespace

std::filesystem::path library_folders_file(const std::filesystem::path& steam) {
  return (steam / "steamapps" / "libraryfolders.vdf").make_preferred();
}

Result<std::vector<std::filesystem::path>> parse_library_folders(std::string_view vdf) {
  const Result<std::vector<KvNode>> document = KvParser{vdf}.parse();
  if (!document) {
    return tl::unexpected(document.error());
  }
  const KvNode* folders = child(*document, "libraryfolders");
  if (folders == nullptr || !folders->object) {
    return fail(Errc::invalid_data, "no \"libraryfolders\" section");
  }
  std::vector<std::filesystem::path> libraries;
  for (const KvNode& entry : folders->children) {
    if (!numbered(entry.key)) {
      continue;
    }
    std::optional<std::string_view> path;
    if (!entry.object) {
      path = entry.value;
    } else if (const KvNode* value = child(entry.children, "path");
               value != nullptr && !value->object) {
      path = value->value;
    }
    if (path && !path->empty()) {
      libraries.push_back(path_from_utf8(*path).make_preferred());
    }
  }
  return libraries;
}

std::filesystem::path steam_common_folder(const std::filesystem::path& library) {
  return library / "steamapps" / "common";
}

}  // namespace myclean::games
