#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

// Typed, validated access to TOML tables, shared by the pack parsers and the pack fixture
// runner (tests/unit/packs). Not part of the module's API.
namespace myclean::packs::detail {

[[nodiscard]] std::string utf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path from_utf8(std::string_view text);

// Thrown at the first schema violation; the parser returns it as Errc::invalid_data.
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The document `text`, or SchemaError "<file>: is not valid TOML (line L, column C): ...".
[[nodiscard]] toml::table parse_document(std::string_view text, const std::filesystem::path& file);

// The keys of one table. Every message starts with `where` ("<file>: relocate #2 'cargo'");
// `prefix` qualifies the fields of a nested table ("current.").
class Fields {
 public:
  Fields(const toml::table& table, std::string where, std::string prefix = {})
      : table_(&table), where_(std::move(where)), prefix_(std::move(prefix)) {}

  [[nodiscard]] const std::string& where() const { return where_; }
  [[nodiscard]] const toml::table& table() const { return *table_; }

  // Throws "<where>: field '<prefix><key>' <problem>", or "<where>: <problem>" for an empty key.
  [[noreturn]] void fail(std::string_view key, std::string_view problem) const;
  // Fails on the first key that is not in `allowed`.
  void only(std::initializer_list<std::string_view> allowed) const;

  [[nodiscard]] std::optional<std::string> string(std::string_view key) const;
  [[nodiscard]] std::string required_string(std::string_view key) const;
  [[nodiscard]] std::optional<bool> boolean(std::string_view key) const;
  [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key, std::int64_t min,
                                                    std::int64_t max) const;
  // Empty when absent; when present, a non-empty list of non-empty strings.
  [[nodiscard]] std::vector<std::string> strings(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> required_strings(std::string_view key) const;
  // Null when absent.
  [[nodiscard]] const toml::table* table(std::string_view key) const;
  // The tables of an array of tables; empty when absent.
  [[nodiscard]] std::vector<const toml::table*> tables(std::string_view key) const;

  // The position in `names` of the required string `key`.
  [[nodiscard]] std::size_t required_index(std::string_view key,
                                           std::span<const std::string_view> names) const;

  // The enumerator in the position of the required string `key` in `names`.
  template <typename Enum>
  [[nodiscard]] Enum required_choice(std::string_view key,
                                     std::span<const std::string_view> names) const {
    return static_cast<Enum>(required_index(key, names));
  }

 private:
  const toml::table* table_;
  std::string where_;
  std::string prefix_;
};

// True when `text` is a path pattern that is absolute once its variables are expanded: it starts
// with a %VARIABLE% or a drive ("C:\", "?:\"), separates components with '\' only, and has no
// empty, "." or ".." component.
[[nodiscard]] bool valid_location(std::string_view text);

}  // namespace myclean::packs::detail
