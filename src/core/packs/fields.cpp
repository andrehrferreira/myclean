#include "core/packs/fields.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

namespace myclean::packs::detail {

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::filesystem::path from_utf8(std::string_view text) {
  return std::filesystem::path{std::u8string(text.begin(), text.end())};
}

toml::table parse_document(std::string_view text, const std::filesystem::path& file) {
  try {
    return toml::parse(text, utf8(file));
  } catch (const toml::parse_error& error) {
    const toml::source_position& at = error.source().begin;
    throw SchemaError(utf8(file) + ": is not valid TOML (line " + std::to_string(at.line) +
                      ", column " + std::to_string(at.column) +
                      "): " + std::string(error.description()));
  }
}

void Fields::fail(std::string_view key, std::string_view problem) const {
  if (key.empty()) {
    throw SchemaError(where_ + ": " + std::string(problem));
  }
  throw SchemaError(where_ + ": field '" + prefix_ + std::string(key) + "' " +
                    std::string(problem));
}

void Fields::only(std::initializer_list<std::string_view> allowed) const {
  for (const auto& [key, node] : *table_) {
    if (std::ranges::find(allowed, key.str()) == allowed.end()) {
      fail(key.str(), "is not a known field");
    }
  }
}

std::optional<std::string> Fields::string(std::string_view key) const {
  const toml::node* node = table_->get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (!node->is_string() || node->as_string()->get().empty()) {
    fail(key, "must be a non-empty string");
  }
  return node->as_string()->get();
}

std::string Fields::required_string(std::string_view key) const {
  std::optional<std::string> value = string(key);
  if (!value) {
    fail(key, "is missing");
  }
  return std::move(*value);
}

std::size_t Fields::required_index(std::string_view key,
                                   std::span<const std::string_view> names) const {
  const std::string text = required_string(key);
  const auto found = std::ranges::find(names, text);
  if (found == names.end()) {
    std::string choices;
    for (const std::string_view name : names) {
      choices += (choices.empty() ? "" : ", ") + std::string(name);
    }
    fail(key, "must be one of " + choices + ", not '" + text + "'");
  }
  return static_cast<std::size_t>(found - names.begin());
}

std::optional<bool> Fields::boolean(std::string_view key) const {
  const toml::node* node = table_->get(key);
  if (node == nullptr) {
    return std::nullopt;
  }
  if (!node->is_boolean()) {
    fail(key, "must be true or false");
  }
  return node->as_boolean()->get();
}

std::optional<std::int64_t> Fields::integer(std::string_view key, std::int64_t min,
                                            std::int64_t max) const {
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
  return value;
}

std::vector<std::string> Fields::strings(std::string_view key) const {
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

std::vector<std::string> Fields::required_strings(std::string_view key) const {
  if (table_->get(key) == nullptr) {
    fail(key, "is missing");
  }
  return strings(key);
}

const toml::table* Fields::table(std::string_view key) const {
  const toml::node* node = table_->get(key);
  if (node != nullptr && !node->is_table()) {
    fail(key, "must be a table");
  }
  return node == nullptr ? nullptr : node->as_table();
}

std::vector<const toml::table*> Fields::tables(std::string_view key) const {
  const toml::node* node = table_->get(key);
  if (node == nullptr) {
    return {};
  }
  if (!node->is_array_of_tables()) {
    fail(key, "must be an array of tables ([[" + std::string(key) + "]])");
  }
  std::vector<const toml::table*> result;
  for (const toml::node& element : *node->as_array()) {
    result.push_back(element.as_table());
  }
  return result;
}

bool valid_location(std::string_view text) {
  const bool variable = text.starts_with('%');
  const bool drive = text.size() >= 3 && text[1] == ':' && text[2] == '\\';
  if ((!variable && !drive) || text.find('/') != std::string_view::npos || text.ends_with('\\')) {
    return false;
  }
  std::size_t start = drive ? 3 : 0;
  while (start <= text.size()) {
    const std::size_t end = std::min(text.find('\\', start), text.size());
    const std::string_view component = text.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace myclean::packs::detail
