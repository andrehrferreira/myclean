#include "core/games/owner_games.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/result.hpp"

namespace myclean::games {

namespace {

constexpr std::string_view file_name = "games.toml";

// The strings of list `key` (none when it is absent); nullopt when it is not a list of non-empty
// strings.
std::optional<std::vector<std::string>> strings(const toml::table& root, std::string_view key) {
  if (!root.contains(key)) {
    return std::vector<std::string>{};
  }
  const toml::array* array = root[key].as_array();
  if (array == nullptr) {
    return std::nullopt;
  }
  std::vector<std::string> values;
  for (const toml::node& element : *array) {
    if (!element.is_string() || element.as_string()->get().empty()) {
      return std::nullopt;
    }
    values.push_back(element.as_string()->get());
  }
  return values;
}

tl::unexpected<Error> field_error(std::string_view key, std::string_view problem) {
  return fail(Errc::invalid_data, std::string(file_name) + ": field '" + std::string(key) + "' " +
                                      std::string(problem));
}

}  // namespace

Result<OwnerGames> parse_owner_games(std::string_view text) {
  toml::table root;
  try {
    root = toml::parse(text, file_name);
  } catch (const toml::parse_error& error) {
    return fail(Errc::invalid_data, std::string(file_name) + " is not valid TOML (line " +
                                        std::to_string(error.source().begin.line) +
                                        "): " + std::string(error.description()));
  }
  for (const auto& [key, node] : root) {
    if (key.str() != "owner" && key.str() != "roots") {
      return field_error(key.str(), "is not a known field");
    }
  }
  OwnerGames games;
  std::optional<std::vector<std::string>> owner = strings(root, "owner");
  if (!owner) {
    return field_error("owner", "must be a list of non-empty strings");
  }
  games.owner = *std::move(owner);
  const std::optional<std::vector<std::string>> roots = strings(root, "roots");
  if (!roots) {
    return field_error("roots", "must be a list of non-empty strings");
  }
  for (const std::string& entry : *roots) {
    const std::filesystem::path path{std::u8string(entry.begin(), entry.end())};
    if (!path.has_root_name() || !path.has_root_directory() || path.native().starts_with(L"\\\\")) {
      return field_error("roots", "must hold absolute drive paths, not '" + entry + "'");
    }
    games.roots.push_back(path);
  }
  return games;
}

}  // namespace myclean::games
