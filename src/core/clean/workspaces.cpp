#include "core/clean/workspaces.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/result.hpp"
#include "core/safety/paths.hpp"

namespace myclean::clean {

namespace {

namespace fs = std::filesystem;

std::filesystem::path from_utf8(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// The strings of list `key`; nullopt (and `problem` set) when it is not a non-empty list of
// non-empty strings.
std::optional<std::vector<std::string>> strings(const toml::table& root, std::string_view key,
                                                std::string& problem) {
  const toml::array* array = root[key].as_array();
  if (array == nullptr || array->empty()) {
    problem = "must be a non-empty list of strings";
    return std::nullopt;
  }
  std::vector<std::string> values;
  for (const toml::node& element : *array) {
    if (!element.is_string() || element.as_string()->get().empty()) {
      problem = "must be a non-empty list of non-empty strings";
      return std::nullopt;
    }
    values.push_back(element.as_string()->get());
  }
  return values;
}

}  // namespace

Result<Workspaces> parse_workspace_file(std::string_view text, const fs::path& file) {
  const std::string where = safety::to_utf8(file) + ": ";
  toml::table root;
  try {
    root = toml::parse(text, safety::to_utf8(file));
  } catch (const toml::parse_error& error) {
    return fail(Errc::invalid_data, where + "is not valid TOML (line " +
                                        std::to_string(error.source().begin.line) +
                                        "): " + std::string(error.description()));
  }
  auto field_error = [&where](std::string_view key, std::string_view problem) {
    return fail(Errc::invalid_data,
                where + "field '" + std::string(key) + "' " + std::string(problem));
  };
  for (const auto& [key, node] : root) {
    if (key.str() != "roots" && key.str() != "skip" && key.str() != "max_depth") {
      return field_error(key.str(), "is not a known field");
    }
  }

  Workspaces workspaces;
  std::string problem;
  const std::optional<std::vector<std::string>> roots = strings(root, "roots", problem);
  if (!roots) {
    return field_error("roots", root.contains("roots") ? problem : "is missing");
  }
  for (const std::string& text_root : *roots) {
    const fs::path path = from_utf8(text_root);
    if (!safety::normalize_path(path)) {
      return field_error("roots", "must hold absolute drive paths, not '" + text_root + "'");
    }
    workspaces.roots.push_back(path);
  }
  if (root.contains("skip")) {
    const std::optional<std::vector<std::string>> skip = strings(root, "skip", problem);
    if (!skip) {
      return field_error("skip", problem);
    }
    for (const std::string& name : *skip) {
      workspaces.skip.push_back(from_utf8(name).native());
    }
  }
  if (const toml::node* depth = root.get("max_depth")) {
    constexpr std::int64_t max_depth = 64;
    if (!depth->is_integer() || depth->as_integer()->get() < 1 ||
        depth->as_integer()->get() > max_depth) {
      return field_error("max_depth", "must be an integer between 1 and 64");
    }
    workspaces.max_depth = static_cast<int>(depth->as_integer()->get());
  }
  return workspaces;
}

}  // namespace myclean::clean
