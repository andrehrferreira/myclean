#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/rules/schema.hpp"

// Shared by the parsers in parse_toml.cpp and parse_kb.cpp; not part of the module's API.
namespace myclean::rules::detail {

[[nodiscard]] std::string utf8(const std::filesystem::path& path);

// Thrown inside a parser at the first schema violation; the parser returns it as invalid_data.
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Where a violation is: the file, or one entry of it ("<file>: rule #3 'dev.node'").
class Location {
 public:
  explicit Location(const std::filesystem::path& file);
  Location(const std::filesystem::path& file, std::string_view kind, std::size_t number);

  // Adds the entry's id to every later message.
  void set_id(std::string_view id);
  // Throws SchemaError "<where>: field '<field>' <problem>", or "<where>: <problem>" when
  // `field` is empty.
  [[noreturn]] void fail(std::string_view field, std::string_view problem) const;

 private:
  std::string where_;
};

// Compiles a data-file regular expression; a malformed one fails `field` of `location`.
[[nodiscard]] Pattern compile_pattern(const std::string& text, const Location& location,
                                      std::string_view field);

// Records `id`, failing `location` when an earlier entry of the same file already used it.
void claim_id(std::set<std::string, std::less<>>& ids, const std::string& id,
              const Location& location);

}  // namespace myclean::rules::detail
