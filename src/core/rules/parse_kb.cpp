// Parser of the knowledge-base files: data/kb/*.json.

#include <algorithm>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/parse_support.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {

namespace {

using detail::Location;
using detail::SchemaError;
using Json = nlohmann::json;

// Typed, validated access to the members of one JSON object.
class Fields {
 public:
  Fields(const Json& object, const Location& location) : object_(&object), location_(&location) {}

  // Fails on the first member that is not in `allowed`.
  void only(std::initializer_list<std::string_view> allowed) const {
    for (const auto& [key, value] : object_->items()) {
      if (std::ranges::find(allowed, key) == allowed.end()) {
        location_->fail(key, "is not a known field");
      }
    }
  }

  [[nodiscard]] std::optional<std::string> string(std::string_view key) const {
    const auto member = object_->find(key);
    if (member == object_->end()) {
      return std::nullopt;
    }
    if (!member->is_string() || member->get_ref<const std::string&>().empty()) {
      location_->fail(key, "must be a non-empty string");
    }
    return member->get<std::string>();
  }

  [[nodiscard]] std::string required_string(std::string_view key) const {
    std::optional<std::string> value = string(key);
    if (!value) {
      location_->fail(key, "is missing");
    }
    return std::move(*value);
  }

  [[nodiscard]] bool required_boolean(std::string_view key) const {
    const auto member = object_->find(key);
    if (member == object_->end()) {
      location_->fail(key, "is missing");
    }
    if (!member->is_boolean()) {
      location_->fail(key, "must be true or false");
    }
    return member->get<bool>();
  }

 private:
  const Json* object_;
  const Location* location_;
};

KbEntry parse_entry(const Json& object, Location& location) {
  if (!object.is_object()) {
    location.fail("", "must be a JSON object");
  }
  const Fields fields(object, location);
  KbEntry entry;
  entry.id = fields.required_string("id");
  location.set_id(entry.id);
  fields.only({"id", "image", "signer", "path", "description", "category", "needed_at_startup",
               "safe_to_remove"});
  entry.image = fields.required_string("image");
  entry.signer = fields.string("signer");
  if (const std::optional<std::string> path = fields.string("path")) {
    entry.path = detail::compile_pattern(*path, location, "path");
  }
  entry.description = fields.required_string("description");
  entry.category = fields.required_string("category");
  entry.needed_at_startup = fields.required_boolean("needed_at_startup");
  entry.safe_to_remove = fields.required_boolean("safe_to_remove");
  return entry;
}

}  // namespace

Result<std::vector<KbEntry>> parse_kb_file(std::string_view text,
                                           const std::filesystem::path& file) {
  try {
    const Location file_location(file);
    Json root;
    try {
      root = Json::parse(text);
    } catch (const Json::parse_error& error) {
      file_location.fail("", "is not valid JSON: " + std::string(error.what()));
    }
    if (!root.is_object()) {
      file_location.fail("", "must be a JSON object with an \"entries\" array");
    }
    Fields(root, file_location).only({"entries"});
    const auto entries = root.find("entries");
    if (entries == root.end() || !entries->is_array()) {
      file_location.fail("entries", "must be an array");
    }
    std::vector<KbEntry> result;
    std::set<std::string, std::less<>> ids;
    for (const Json& object : *entries) {
      Location location(file, "entry", result.size() + 1);
      result.push_back(parse_entry(object, location));
      detail::claim_id(ids, result.back().id, location);
    }
    return result;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

}  // namespace myclean::rules
