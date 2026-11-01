#include "core/games/epic.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/games/paths.hpp"
#include "core/model/result.hpp"

namespace myclean::games {

namespace {

using nlohmann::json;

// The string field `name` of `object` into `out`; false when present but not a string.
bool read_string(const json& object, const char* name, std::string& out) {
  const auto found = object.find(name);
  if (found == object.end() || found->is_null()) {
    return true;
  }
  if (!found->is_string()) {
    return false;
  }
  out = found->get<std::string>();
  return true;
}

// True when a category is `name` or starts with "<name>/": "engines/ue5" is an engines one.
bool has_category(const std::vector<std::string>& categories, std::string_view name) {
  return std::ranges::any_of(categories, [name](std::string_view category) {
    return category.starts_with(name) &&
           (category.size() == name.size() || category[name.size()] == '/');
  });
}

}  // namespace

Result<EpicManifest> parse_epic_manifest(std::string_view text) {
  const json document = json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return fail(Errc::invalid_data, "not a JSON object");
  }
  EpicManifest manifest;
  std::string location;
  for (const auto& [name, out] :
       {std::pair{"DisplayName", &manifest.display_name}, std::pair{"InstallLocation", &location},
        std::pair{"LaunchExecutable", &manifest.launch_executable}}) {
    if (!read_string(document, name, *out)) {
      return fail(Errc::invalid_data, std::string(name) + " is not a string");
    }
  }
  if (!location.empty()) {
    manifest.install_location = path_from_utf8(location).make_preferred();
  }
  if (const auto categories = document.find("AppCategories");
      categories != document.end() && !categories->is_null()) {
    if (!categories->is_array()) {
      return fail(Errc::invalid_data, "AppCategories is not an array");
    }
    for (const json& category : *categories) {
      if (!category.is_string()) {
        return fail(Errc::invalid_data, "AppCategories holds a value that is not a string");
      }
      manifest.categories.push_back(category.get<std::string>());
    }
  }
  return manifest;
}

bool is_epic_game(const EpicManifest& manifest) {
  return has_category(manifest.categories, "games") &&
         !has_category(manifest.categories, "engines") &&
         !has_category(manifest.categories, "plugins") && !manifest.launch_executable.empty();
}

}  // namespace myclean::games
