#include "core/map/attribution.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::map {

namespace {

constexpr std::array<std::string_view, category_count> category_names{
    "system",         "windows-servicing", "page-hibernate", "restore-points", "apps",
    "games",          "dev-toolchains",    "dev-caches",     "dev-projects",   "build-artifacts",
    "browser-caches", "app-caches",        "vm-disks",       "user-documents", "user-media",
    "downloads",      "recycle-bin",       "other"};

constexpr std::array<std::string_view, 7> reclaim_kind_names{
    "none", "clean-rule", "handler", "relocate", "setting", "uninstall", "manual-review"};

constexpr std::array<std::string_view, 3> setting_refs{"hibernation", "page_file",
                                                       "shadow_storage"};

template <typename Enum, std::size_t Size>
std::optional<Enum> parse_name(const std::array<std::string_view, Size>& names,
                               std::string_view text) {
  const auto found = std::ranges::find(names, text);
  if (found == names.end()) {
    return std::nullopt;
  }
  return static_cast<Enum>(found - names.begin());
}

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// Thrown at the first schema violation; returned as invalid_data.
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Typed access to the keys of one TOML table; every message starts with `where`.
class Fields {
 public:
  Fields(const toml::table& table, std::string where, std::string prefix = {})
      : table_(&table), where_(std::move(where)), prefix_(std::move(prefix)) {}

  [[noreturn]] void fail(std::string_view key, std::string_view problem) const {
    throw SchemaError(where_ + ": field '" + prefix_ + std::string(key) + "' " +
                      std::string(problem));
  }

  void only(std::initializer_list<std::string_view> allowed) const {
    for (const auto& [key, node] : *table_) {
      if (std::ranges::find(allowed, key.str()) == allowed.end()) {
        fail(key.str(), "is not a known field");
      }
    }
  }

  [[nodiscard]] std::optional<std::string> string(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_string() || node->as_string()->get().empty()) {
      fail(key, "must be a non-empty string");
    }
    return node->as_string()->get();
  }

  [[nodiscard]] std::string required_string(std::string_view key) const {
    std::optional<std::string> value = string(key);
    if (!value) {
      fail(key, "is missing");
    }
    return std::move(*value);
  }

  template <typename Enum, std::size_t Size>
  [[nodiscard]] Enum required_choice(std::string_view key,
                                     const std::array<std::string_view, Size>& names) const {
    const std::string text = required_string(key);
    const std::optional<Enum> value = parse_name<Enum>(names, text);
    if (!value) {
      std::string choices;
      for (const std::string_view name : names) {
        choices += (choices.empty() ? "" : ", ") + std::string(name);
      }
      fail(key, "must be one of " + choices + ", not '" + text + "'");
    }
    return *value;
  }

  [[nodiscard]] std::vector<std::string> required_strings(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      fail(key, "is missing");
    }
    const toml::array* array = node->as_array();
    if (array == nullptr || array->empty()) {
      fail(key, "must be a non-empty list of strings");
    }
    std::vector<std::string> values;
    for (const toml::node& element : *array) {
      if (!element.is_string() || element.as_string()->get().empty()) {
        fail(key, "must be a non-empty list of non-empty strings");
      }
      values.push_back(element.as_string()->get());
    }
    return values;
  }

  [[nodiscard]] const toml::table& required_table(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      fail(key, "is missing");
    }
    if (!node->is_table()) {
      fail(key, "must be a table");
    }
    return *node->as_table();
  }

  [[nodiscard]] const std::string& where() const { return where_; }

 private:
  const toml::table* table_;
  std::string where_;
  std::string prefix_;
};

// A location starts with a %VARIABLE% or a drive ("C:\", "?:\"), separates components with '\'
// only, and has no empty, "." or ".." component.
bool valid_location(std::wstring_view text) {
  const bool variable = text.starts_with(L'%');
  const bool drive = text.size() >= 3 && text[1] == L':' && text[2] == L'\\';
  if ((!variable && !drive) || text.find(L'/') != std::wstring_view::npos ||
      text.ends_with(L'\\')) {
    return false;
  }
  std::size_t start = drive ? 3 : 0;
  while (start <= text.size()) {
    const std::size_t end = std::min(text.find(L'\\', start), text.size());
    const std::wstring_view component = text.substr(start, end - start);
    if (component.empty() || component == L"." || component == L"..") {
      return false;
    }
    start = end + 1;
  }
  return true;
}

Reclaim parse_reclaim(const Fields& area_fields) {
  const Fields fields(area_fields.required_table("reclaim"), area_fields.where(), "reclaim.");
  fields.only({"kind", "ref", "note"});
  Reclaim reclaim{.kind = fields.required_choice<ReclaimKind>("kind", reclaim_kind_names),
                  .ref = fields.string("ref").value_or(""),
                  .note = fields.string("note").value_or("")};
  const bool needs_ref =
      reclaim.kind == ReclaimKind::clean_rule || reclaim.kind == ReclaimKind::handler ||
      reclaim.kind == ReclaimKind::relocate || reclaim.kind == ReclaimKind::setting;
  const std::string kind(to_string(reclaim.kind));
  if (needs_ref && reclaim.ref.empty()) {
    fields.fail("ref", "is required for kind '" + kind + "'");
  }
  if (!needs_ref && !reclaim.ref.empty()) {
    fields.fail("ref", "is not allowed for kind '" + kind + "'");
  }
  if (reclaim.kind == ReclaimKind::setting && !parse_name<int>(setting_refs, reclaim.ref)) {
    fields.fail("ref",
                "must be hibernation, page_file or shadow_storage, not '" + reclaim.ref + "'");
  }
  return reclaim;
}

Area parse_area(const toml::table& table, std::string where) {
  Fields fields(table, std::move(where));
  Area area;
  area.id = fields.required_string("id");
  fields = Fields(table, fields.where() + " '" + area.id + "'");
  fields.only({"id", "paths", "category", "label", "reclaim"});
  for (const std::string& text : fields.required_strings("paths")) {
    std::wstring path = std::filesystem::path{std::u8string(text.begin(), text.end())}.native();
    if (!valid_location(path)) {
      fields.fail("paths",
                  "must hold absolute paths starting with a drive or a %VARIABLE%, "
                  "separated by '\\', not '" +
                      text + "'");
    }
    area.paths.push_back(std::move(path));
  }
  area.category = fields.required_choice<Category>("category", category_names);
  area.label = fields.required_string("label");
  area.reclaim = parse_reclaim(fields);
  return area;
}

}  // namespace

std::string_view to_string(Category category) {
  return category_names.at(static_cast<std::size_t>(category));
}

std::optional<Category> parse_category(std::string_view text) {
  return parse_name<Category>(category_names, text);
}

std::string_view to_string(ReclaimKind kind) {
  return reclaim_kind_names.at(static_cast<std::size_t>(kind));
}

std::optional<ReclaimKind> parse_reclaim_kind(std::string_view text) {
  return parse_name<ReclaimKind>(reclaim_kind_names, text);
}

Result<std::vector<Area>> parse_attribution_file(std::string_view text,
                                                 const std::filesystem::path& file) {
  const std::string name = utf8(file);
  toml::table root;
  try {
    root = toml::parse(text, name);
  } catch (const toml::parse_error& error) {
    return fail(Errc::invalid_data, name + ": is not valid TOML (line " +
                                        std::to_string(error.source().begin.line) +
                                        "): " + std::string(error.description()));
  }
  try {
    const Fields root_fields(root, name);
    root_fields.only({"area"});
    const toml::node* node = root.get("area");
    if (node == nullptr || !node->is_array_of_tables() || node->as_array()->empty()) {
      root_fields.fail("area", "must be a non-empty array of tables ([[area]])");
    }
    std::vector<Area> areas;
    std::set<std::string, std::less<>> ids;
    for (const toml::node& element : *node->as_array()) {
      areas.push_back(
          parse_area(*element.as_table(), name + ": area #" + std::to_string(areas.size() + 1)));
      if (!ids.insert(areas.back().id).second) {
        throw SchemaError(name + ": area #" + std::to_string(areas.size()) + " '" +
                          areas.back().id + "': field 'id' repeats an id used earlier in the file");
      }
    }
    return areas;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

Result<std::vector<Area>> load_attribution(const rules::IDataFiles& files,
                                           const std::filesystem::path& data_dir) {
  const std::filesystem::path file = data_dir / "attribution.toml";
  return files.read(file).and_then(
      [&file](const std::string& text) { return parse_attribution_file(text, file); });
}

}  // namespace myclean::map
