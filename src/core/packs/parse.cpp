#include "core/packs/parse.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/packs/fields.hpp"
#include "core/packs/schema.hpp"
#include "core/rules/schema.hpp"

namespace myclean::packs {

namespace {

using detail::Fields;
using detail::SchemaError;

constexpr std::array<std::string_view, 4> setting_kind_names = {"env", "config", "registry",
                                                                "command"};
constexpr std::array<std::string_view, 2> content_mode_names = {"move", "regenerate"};
constexpr std::array<std::string_view, 3> undo_kind_names = {"restore", "command", "none"};
constexpr std::array<std::string_view, 2> origin_names = {"builtin", "owner"};
constexpr std::array<std::string_view, 2> config_format_names = {"ini", "json"};
constexpr std::array<std::string_view, 4> algorithm_names = {"xpress4k", "xpress8k", "xpress16k",
                                                             "lzx"};

// One decimal component without sign or leading zero; nullopt otherwise.
std::optional<int> version_component(std::string_view text) {
  if (text.empty() || text.front() < '0' || text.front() > '9' ||
      (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

Version required_version(const Fields& fields, std::string_view key) {
  const std::string text = fields.required_string(key);
  const std::optional<Version> version = parse_version(text);
  if (!version) {
    fields.fail(key, "must be a version MAJOR.MINOR.PATCH, not '" + text + "'");
  }
  return *version;
}

// A name without a folder: an image, or a file or folder name pattern.
void check_names(const Fields& fields, std::string_view key,
                 const std::vector<std::string>& names) {
  for (const std::string& name : names) {
    if (name.find_first_of("\\/") != std::string::npos) {
      fields.fail(key, "must hold names without a folder, not '" + name + "'");
    }
  }
}

// `any_drive`: the paths only name places, so "?:\" may stand for every drive; otherwise they
// are looked up on the machine and need a real drive or variable.
void check_locations(const Fields& fields, std::string_view key,
                     const std::vector<std::string>& paths, bool any_drive) {
  for (const std::string& path : paths) {
    if (!detail::valid_location(path)) {
      fields.fail(key,
                  "must hold absolute paths starting with a drive or a %VARIABLE%, "
                  "separated by '\\', not '" +
                      path + "'");
    }
    if (!any_drive && path.starts_with("?:")) {
      fields.fail(key, "must name a drive or a %VARIABLE%, not '?:', in '" + path + "'");
    }
  }
}

Condition parse_condition(const toml::table& table, const std::string& where) {
  const Fields fields(table, where);
  fields.only({"app_installed", "path_exists", "process_image", "os_build_min"});
  if (table.size() != 1) {
    fields.fail("",
                "must set exactly one of app_installed, path_exists, process_image and "
                "os_build_min");
  }
  Condition condition;
  if (const std::optional<std::string> app = fields.string("app_installed")) {
    condition.kind = ConditionKind::app_installed;
    condition.text = *app;
    try {
      condition.app = rules::Pattern{
          .text = *app, .regex = std::regex(*app, std::regex::ECMAScript | std::regex::icase)};
    } catch (const std::regex_error& error) {
      fields.fail("app_installed",
                  "is not a valid regular expression: " + std::string(error.what()));
    }
  } else if (const std::optional<std::string> path = fields.string("path_exists")) {
    condition.kind = ConditionKind::path_exists;
    condition.text = *path;
    check_locations(fields, "path_exists", {*path}, false);
  } else if (const std::optional<std::string> image = fields.string("process_image")) {
    condition.kind = ConditionKind::process_image;
    condition.text = *image;
    check_names(fields, "process_image", {*image});
  } else {
    constexpr std::int64_t max_build = 1'000'000;
    condition.kind = ConditionKind::os_build_min;
    condition.build = static_cast<std::uint32_t>(*fields.integer("os_build_min", 1, max_build));
  }
  return condition;
}

std::vector<Condition> parse_conditions(const Fields& fields, std::string_view key) {
  const toml::node* node = fields.table().get(key);
  if (node == nullptr) {
    return {};
  }
  if (!node->is_array_of_tables() || node->as_array()->empty()) {
    fields.fail(key, "must be a non-empty list of conditions ({ path_exists = '...' })");
  }
  std::vector<Condition> conditions;
  for (const toml::node& element : *node->as_array()) {
    conditions.push_back(
        parse_condition(*element.as_table(), fields.where() + ": applies_when." + std::string(key) +
                                                 " #" + std::to_string(conditions.size() + 1)));
  }
  return conditions;
}

bool valid_pack_id(std::string_view id) {
  return !id.empty() && !id.starts_with('-') && !id.ends_with('-') &&
         std::ranges::all_of(id, [](char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-';
         });
}

Setting parse_setting(const Fields& recipe) {
  const toml::table* table = recipe.table("current");
  if (table == nullptr) {
    recipe.fail("current", "is missing");
  }
  const Fields fields(*table, recipe.where(), "current.");
  fields.only({"env", "config", "key", "format", "registry", "name", "command"});
  Setting setting;
  int kinds = 0;
  for (std::size_t index = 0; index < setting_kind_names.size(); ++index) {
    if (std::optional<std::string> text = fields.string(setting_kind_names.at(index))) {
      setting.kind = static_cast<SettingKind>(index);
      setting.text = std::move(*text);
      ++kinds;
    }
  }
  if (kinds != 1) {
    recipe.fail("current", "must set exactly one of env, config, registry and command");
  }
  const bool config = setting.kind == SettingKind::config;
  const bool registry = setting.kind == SettingKind::registry;
  if (config) {
    check_locations(fields, "config", {setting.text}, false);
    setting.key = fields.required_string("key");
    setting.format =
        std::string(config_format_names.at(fields.required_index("format", config_format_names)));
  } else {
    for (const std::string_view key : {"key", "format"}) {
      if (table->contains(key)) {
        fields.fail(key, "is only allowed with config");
      }
    }
  }
  if (registry && !setting.text.starts_with("HKCU\\") && !setting.text.starts_with("HKLM\\")) {
    fields.fail("registry", "must start with HKCU\\ or HKLM\\, not '" + setting.text + "'");
  }
  setting.name = fields.string("name").value_or("");
  if (!registry && !setting.name.empty()) {
    fields.fail("name", "is only allowed with registry");
  }
  return setting;
}

// Every {placeholder} of `command` is {target} or, when the setting names instances, {name};
// {target} is required.
void check_command(const Fields& fields, const std::string& command, const Setting& current) {
  bool target = false;
  for (std::size_t open = command.find('{'); open != std::string::npos;
       open = command.find('{', open + 1)) {
    const std::size_t close = command.find('}', open);
    const std::string_view name = std::string_view(command).substr(
        open + 1, close == std::string::npos ? 0 : close - open - 1);
    target = target || name == "target";
    if (name != "target" && (name != "name" || current.name.empty())) {
      fields.fail("command", current.name.empty()
                                 ? "may only hold the placeholder {target}"
                                 : "may only hold the placeholders {target} and {name}");
    }
  }
  if (!target) {
    fields.fail("command", "must hold the placeholder {target}");
  }
}

RelocationRecipe parse_recipe(const toml::table& table, const std::string& position) {
  RelocationRecipe recipe;
  recipe.id = Fields(table, position).required_string("id");
  const Fields fields(table, position + " '" + recipe.id + "'");
  fields.only({"id", "label", "locations", "current", "target_kind", "command", "content", "needs",
               "skip_names", "undo", "note"});
  recipe.label = fields.required_string("label");
  recipe.locations = fields.required_strings("locations");
  check_locations(fields, "locations", recipe.locations, true);
  recipe.current = parse_setting(fields);
  recipe.target_kind = fields.required_choice<SettingKind>("target_kind", setting_kind_names);
  recipe.command = fields.string("command").value_or("");
  if (recipe.target_kind == SettingKind::command) {
    if (recipe.command.empty()) {
      fields.fail("command", "is required for target_kind 'command'");
    }
    check_command(fields, recipe.command, recipe.current);
  } else {
    if (!recipe.command.empty()) {
      fields.fail("command", "is only allowed with target_kind 'command'");
    }
    if (recipe.target_kind != recipe.current.kind) {
      fields.fail("target_kind", "must be '" + std::string(to_string(recipe.current.kind)) +
                                     "': it writes the setting `current` names");
    }
  }
  recipe.content = fields.required_choice<ContentMode>("content", content_mode_names);
  recipe.needs = fields.strings("needs");
  check_names(fields, "needs", recipe.needs);
  recipe.skip_names = fields.strings("skip_names");
  if (!recipe.skip_names.empty() && recipe.current.name.empty()) {
    fields.fail("skip_names", "needs instances: current.registry with current.name");
  }
  recipe.undo = fields.required_choice<UndoKind>("undo", undo_kind_names);
  recipe.note = fields.string("note").value_or("");
  if (recipe.undo == UndoKind::restore && recipe.target_kind == SettingKind::command) {
    fields.fail("undo", "must be command or none for target_kind 'command'");
  }
  if (recipe.undo == UndoKind::command && recipe.target_kind != SettingKind::command) {
    fields.fail("undo", "may be command only for target_kind 'command'");
  }
  if (recipe.undo == UndoKind::none && recipe.note.empty()) {
    fields.fail("note", "is required with undo 'none': it says how to go back");
  }
  return recipe;
}

CompressTarget parse_target(const toml::table& table, const std::string& position) {
  CompressTarget target;
  target.id = Fields(table, position).required_string("id");
  const Fields fields(table, position + " '" + target.id + "'");
  fields.only({"id", "label", "paths", "algorithm", "exclude", "note"});
  target.label = fields.required_string("label");
  target.paths = fields.required_strings("paths");
  check_locations(fields, "paths", target.paths, false);
  target.algorithm =
      std::string(algorithm_names.at(fields.required_index("algorithm", algorithm_names)));
  target.exclude = fields.strings("exclude");
  check_names(fields, "exclude", target.exclude);
  target.note = fields.string("note").value_or("");
  return target;
}

// The entries of the array of tables `key` of a whole file, each parsed by `parse`; ids are
// unique.
template <typename Entry, typename Parse>
Result<std::vector<Entry>> parse_entries(std::string_view text, const std::filesystem::path& file,
                                         std::string_view key, Parse parse) {
  try {
    const std::string name = detail::utf8(file);
    const toml::table root = detail::parse_document(text, file);
    const Fields root_fields(root, name);
    root_fields.only({key});
    const std::vector<const toml::table*> tables = root_fields.tables(key);
    if (tables.empty()) {
      root_fields.fail(key, "must be a non-empty array of tables ([[" + std::string(key) + "]])");
    }
    std::vector<Entry> entries;
    std::set<std::string, std::less<>> ids;
    for (const toml::table* table : tables) {
      const std::string position =
          name + ": " + std::string(key) + " #" + std::to_string(entries.size() + 1);
      entries.push_back(parse(*table, position));
      if (!ids.insert(entries.back().id).second) {
        throw SchemaError(position + " '" + entries.back().id +
                          "': field 'id' repeats an id used earlier in the file");
      }
    }
    return entries;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
  const std::size_t first = text.find('.');
  const std::size_t second = first == std::string_view::npos ? first : text.find('.', first + 1);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<int> major = version_component(text.substr(0, first));
  const std::optional<int> minor = version_component(text.substr(first + 1, second - first - 1));
  const std::optional<int> patch = version_component(text.substr(second + 1));
  if (!major || !minor || !patch) {
    return std::nullopt;
  }
  return Version{.major = *major, .minor = *minor, .patch = *patch};
}

std::string to_string(const Version& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
         std::to_string(version.patch);
}

std::string_view to_string(SettingKind kind) {
  return myclean::detail::enum_name(setting_kind_names, kind);
}

std::string_view to_string(ContentMode mode) {
  return myclean::detail::enum_name(content_mode_names, mode);
}

std::string_view to_string(UndoKind kind) {
  return myclean::detail::enum_name(undo_kind_names, kind);
}

std::string_view to_string(Origin origin) {
  return myclean::detail::enum_name(origin_names, origin);
}

Result<Manifest> parse_manifest(std::string_view text, const std::filesystem::path& file) {
  try {
    const std::string name = detail::utf8(file);
    const toml::table root = detail::parse_document(text, file);
    const Fields fields(root, name);
    fields.only(
        {"id", "version", "title", "description", "sources", "min_myclean", "applies_when"});
    Manifest manifest;
    manifest.id = fields.required_string("id");
    if (!valid_pack_id(manifest.id)) {
      fields.fail("id", "must hold only a-z, 0-9 and inner '-', not '" + manifest.id + "'");
    }
    manifest.version = required_version(fields, "version");
    manifest.title = fields.required_string("title");
    manifest.description = fields.required_string("description");
    manifest.sources = fields.required_strings("sources");
    manifest.min_myclean = required_version(fields, "min_myclean");
    if (const toml::table* applies = fields.table("applies_when")) {
      const Fields applies_fields(*applies, name, "applies_when.");
      applies_fields.only({"all", "any"});
      manifest.applies_when.all = parse_conditions(applies_fields, "all");
      manifest.applies_when.any = parse_conditions(applies_fields, "any");
      if (manifest.applies_when.all.empty() && manifest.applies_when.any.empty()) {
        fields.fail("applies_when", "must set all, any or both");
      }
    }
    return manifest;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

Result<std::vector<RelocationRecipe>> parse_relocate_file(std::string_view text,
                                                          const std::filesystem::path& file) {
  return parse_entries<RelocationRecipe>(text, file, "relocate", parse_recipe);
}

Result<std::vector<CompressTarget>> parse_compress_file(std::string_view text,
                                                        const std::filesystem::path& file) {
  return parse_entries<CompressTarget>(text, file, "compress", parse_target);
}

}  // namespace myclean::packs
