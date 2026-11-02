// Parsers of the TOML data files: data/rules/*.toml and data/classes.toml.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/parse_support.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {

namespace {

using detail::Location;
using detail::SchemaError;

// Typed, validated access to the keys of one TOML table. `prefix` qualifies the field names of
// a nested table in messages ("require.inactive_days").
class Fields {
 public:
  Fields(const toml::table& table, const Location& location, std::string prefix = {})
      : table_(&table), location_(&location), prefix_(std::move(prefix)) {}

  [[noreturn]] void fail(std::string_view key, std::string_view problem) const {
    location_->fail(prefix_ + std::string(key), problem);
  }

  // Fails on the first key that is not in `allowed`.
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
    if (!node->is_string()) {
      fail(key, "must be a string");
    }
    std::string value = node->as_string()->get();
    if (value.empty()) {
      fail(key, "must not be empty");
    }
    return value;
  }

  [[nodiscard]] std::string required_string(std::string_view key) const {
    std::optional<std::string> value = string(key);
    if (!value) {
      fail(key, "is missing");
    }
    return std::move(*value);
  }

  // A required string naming one value of an enumeration; `parse` returns std::nullopt for
  // text that names none, and `choices` lists the valid texts for the message.
  template <typename Parse>
  [[nodiscard]] auto required_choice(std::string_view key, Parse parse,
                                     std::string_view choices) const {
    const std::string text = required_string(key);
    const auto value = parse(text);
    if (!value) {
      fail(key, "must be one of " + std::string(choices) + ", not '" + text + "'");
    }
    return *value;
  }

  [[nodiscard]] std::optional<bool> boolean(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_boolean()) {
      fail(key, "must be true or false");
    }
    return node->as_boolean()->get();
  }

  [[nodiscard]] std::optional<int> integer(std::string_view key, int min, int max) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_integer()) {
      fail(key, "must be an integer");
    }
    const std::int64_t value = node->as_integer()->get();
    if (value < min || value > max) {
      fail(key, "must be between " + std::to_string(min) + " and " + std::to_string(max));
    }
    return static_cast<int>(value);
  }

  // Empty when absent; when present, a non-empty list of non-empty strings.
  [[nodiscard]] std::vector<std::string> string_list(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return {};
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

  // Null when absent.
  [[nodiscard]] const toml::table* table(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node != nullptr && !node->is_table()) {
      fail(key, "must be a table");
    }
    return node == nullptr ? nullptr : node->as_table();
  }

  // The tables of an array of tables ([[key]]); empty when absent.
  [[nodiscard]] std::vector<const toml::table*> table_list(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return {};
    }
    if (!node->is_array_of_tables()) {
      fail(key, "must be an array of tables ([[" + std::string(key) + "]])");
    }
    std::vector<const toml::table*> tables;
    for (const toml::node& element : *node->as_array()) {
      tables.push_back(element.as_table());
    }
    return tables;
  }

 private:
  const toml::table* table_;
  const Location* location_;
  std::string prefix_;
};

toml::table parse_document(std::string_view text, const std::filesystem::path& file) {
  try {
    return toml::parse(text, detail::utf8(file));
  } catch (const toml::parse_error& error) {
    const toml::source_position& where = error.source().begin;
    Location(file).fail("", "is not valid TOML (line " + std::to_string(where.line) + ", column " +
                                std::to_string(where.column) +
                                "): " + std::string(error.description()));
  }
}

std::optional<ExecutionContext> parse_scope(std::string_view text) {
  if (text == "user") {
    return ExecutionContext::user;
  }
  if (text == "system") {
    return ExecutionContext::service;
  }
  return std::nullopt;
}

// `value` is set exactly when the rule's strategy is `owner`; fails `key` otherwise.
void check_strategy_field(const Fields& fields, std::string_view key, const std::string& value,
                          const CleaningRule& rule, Strategy owner) {
  const std::string owner_text(to_string(owner));
  if (rule.strategy == owner && value.empty()) {
    fields.fail(key, "is required for strategy '" + owner_text + "'");
  }
  if (rule.strategy != owner && !value.empty()) {
    fields.fail(key, "is only allowed with strategy '" + owner_text + "'");
  }
}

// True when `text` starts with a drive ("C:\") or "?:\" (every fixed volume).
bool starts_with_drive(const std::string& text) {
  return text.size() >= 3 && text[1] == ':' && (text[2] == '\\' || text[2] == '/') &&
         (text[0] == '?' || (text[0] >= 'A' && text[0] <= 'Z') ||
          (text[0] >= 'a' && text[0] <= 'z'));
}

// True when `text` starts with a drive, a drive-relative path or a separator.
bool rooted(const std::string& text) {
  return (text.size() >= 2 && text[1] == ':') || text.starts_with('\\') || text.starts_with('/');
}

bool has_parent_component(const std::string& text) {
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t end = std::min(text.find_first_of("\\/", start), text.size());
    if (text.substr(start, end - start) == "..") {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// Fails `targets` on a target the engine would never match, so that a rule cannot load and
// silently select nothing: a project rule's targets are relative to the project root; any other
// rule's start with a %VARIABLE%, a drive ("C:\") or "?:\" (every fixed volume). No target has
// a ".." component.
void check_targets(const Fields& fields, const CleaningRule& rule) {
  for (const std::string& target : rule.targets) {
    if (rule.project_marker && (rooted(target) || target.starts_with('%'))) {
      fields.fail("targets", "must hold paths relative to the project root, not '" + target + "'");
    }
    if (!rule.project_marker && !starts_with_drive(target) && !target.starts_with('%')) {
      fields.fail("targets",
                  "must hold absolute paths starting with a drive, '?:\\' or a %VARIABLE%, not '" +
                      target + "'");
    }
    if (has_parent_component(target)) {
      fields.fail("targets", "must not hold a '..' component, in '" + target + "'");
    }
  }
}

// Fails require.contains unless it names one file by a path relative to the selected folder
// ("CMakeCache.txt", "disk\docker_data.vhdx"), and require.outside_of unless it is an absolute
// path without wildcards (so not "?:\") of a rule without project.
void check_requirements(const Fields& fields, const CleaningRule& rule) {
  if (const std::optional<std::string>& contains = rule.require.contains;
      contains &&
      (rooted(*contains) || contains->starts_with('%') ||
       contains->find_first_of("*?") != std::string::npos || has_parent_component(*contains))) {
    fields.fail("contains", "must name one file by a path relative to the selected folder");
  }
  if (const std::optional<std::string>& outside_of = rule.require.outside_of;
      outside_of &&
      (rule.project_marker || (!starts_with_drive(*outside_of) && !outside_of->starts_with('%')) ||
       outside_of->find_first_of("*?") != std::string::npos || has_parent_component(*outside_of))) {
    fields.fail("outside_of",
                "is only allowed on a rule without project, as an absolute path without "
                "wildcards starting with a drive or a %VARIABLE%");
  }
}

CleaningRule parse_rule(const toml::table& table, Location& location) {
  const Fields fields(table, location);
  CleaningRule rule;
  rule.id = fields.required_string("id");
  location.set_id(rule.id);
  fields.only({"id", "category", "tier", "scope", "strategy", "targets", "command", "handler",
               "project", "require"});
  rule.category = fields.required_string("category");
  rule.tier = fields.required_choice("tier", parse_tier, "auto, ask or report");
  rule.scope = fields.required_choice("scope", parse_scope, "user or system");
  rule.strategy =
      fields.required_choice("strategy", parse_strategy, "delete, quarantine, native or handler");
  rule.targets = fields.string_list("targets");
  rule.command = fields.string("command").value_or("");
  rule.handler = fields.string("handler").value_or("");

  if ((rule.strategy == Strategy::remove || rule.strategy == Strategy::quarantine) &&
      rule.targets.empty()) {
    fields.fail("targets",
                "is required for strategy '" + std::string(to_string(rule.strategy)) + "'");
  }
  check_strategy_field(fields, "command", rule.command, rule, Strategy::native);
  check_strategy_field(fields, "handler", rule.handler, rule, Strategy::handler);

  if (const toml::table* project = fields.table("project")) {
    const Fields project_fields(*project, location, "project.");
    project_fields.only({"marker"});
    rule.project_marker = project_fields.required_string("marker");
  }
  check_targets(fields, rule);
  if (const toml::table* require = fields.table("require")) {
    const Fields require_fields(*require, location, "require.");
    require_fields.only({"git_ignored", "outside_git", "not_in_use", "inactive_days",
                         "min_age_days", "contains", "outside_of"});
    constexpr int max_days = 36500;
    rule.require.git_ignored = require_fields.boolean("git_ignored").value_or(false);
    rule.require.outside_git = require_fields.boolean("outside_git").value_or(false);
    if (rule.require.outside_git && (!rule.project_marker || !rule.require.git_ignored)) {
      require_fields.fail("outside_git",
                          "is only allowed on a project rule with require.git_ignored");
    }
    rule.require.not_in_use = require_fields.boolean("not_in_use").value_or(false);
    rule.require.inactive_days = require_fields.integer("inactive_days", 0, max_days);
    rule.require.min_age_days = require_fields.integer("min_age_days", 0, max_days);
    rule.require.contains = require_fields.string("contains");
    rule.require.outside_of = require_fields.string("outside_of");
    check_requirements(require_fields, rule);
  }
  return rule;
}

ClassMatch parse_match(const toml::table& table, Location& location) {
  const Fields fields(table, location);
  ClassMatch match;
  match.id = fields.required_string("id");
  location.set_id(match.id);
  fields.only({"id", "class", "image", "command_line", "path_root", "signer"});
  match.process_class = fields.required_choice("class", parse_process_class,
                                               "system, game, allow, build, test or default");
  match.images = fields.string_list("image");
  for (const std::string& image : match.images) {
    if (image.find_first_of("\\/") != std::string::npos) {
      fields.fail("image", "must hold image names without a folder, not '" + image + "'");
    }
  }
  if (const std::optional<std::string> command_line = fields.string("command_line")) {
    match.command_line = detail::compile_pattern(*command_line, location, "command_line");
  }
  match.path_roots = fields.string_list("path_root");
  match.signers = fields.string_list("signer");
  if (match.images.empty() && !match.command_line && match.path_roots.empty() &&
      match.signers.empty()) {
    location.fail("", "sets none of image, command_line, path_root and signer");
  }
  return match;
}

// Overrides the defaults in `caps` with the [caps.<class_name>] table, when there is one.
void parse_caps(const Fields& caps_fields, const Location& location, const std::string& class_name,
                JobCaps& caps) {
  const toml::table* table = caps_fields.table(class_name);
  if (table == nullptr) {
    return;
  }
  const Fields fields(*table, location, "caps." + class_name + ".");
  fields.only({"cpu_percent", "commit_gib"});
  constexpr int max_commit_gib = 1 << 20;
  caps.cpu_percent = fields.integer("cpu_percent", 1, 100).value_or(caps.cpu_percent);
  if (const std::optional<int> commit_gib = fields.integer("commit_gib", 1, max_commit_gib)) {
    caps.commit = Bytes{static_cast<std::uint64_t>(*commit_gib) << 30U};
  }
}

}  // namespace

Result<std::vector<CleaningRule>> parse_rule_file(std::string_view text,
                                                  const std::filesystem::path& file) {
  try {
    const toml::table root = parse_document(text, file);
    const Location file_location(file);
    const Fields root_fields(root, file_location);
    root_fields.only({"rule"});
    std::vector<CleaningRule> rules;
    std::set<std::string, std::less<>> ids;
    for (const toml::table* table : root_fields.table_list("rule")) {
      Location location(file, "rule", rules.size() + 1);
      rules.push_back(parse_rule(*table, location));
      detail::claim_id(ids, rules.back().id, location);
    }
    return rules;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

Result<ClassSet> parse_class_file(std::string_view text, const std::filesystem::path& file) {
  try {
    const toml::table root = parse_document(text, file);
    const Location file_location(file);
    const Fields root_fields(root, file_location);
    root_fields.only({"match", "caps"});
    ClassSet classes;
    std::set<std::string, std::less<>> ids;
    for (const toml::table* table : root_fields.table_list("match")) {
      Location location(file, "match", classes.matches.size() + 1);
      classes.matches.push_back(parse_match(*table, location));
      detail::claim_id(ids, classes.matches.back().id, location);
    }
    if (const toml::table* caps = root_fields.table("caps")) {
      const Fields caps_fields(*caps, file_location, "caps.");
      caps_fields.only({"build", "test"});
      parse_caps(caps_fields, file_location, "build", classes.build);
      parse_caps(caps_fields, file_location, "test", classes.test);
    }
    return classes;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

}  // namespace myclean::rules
