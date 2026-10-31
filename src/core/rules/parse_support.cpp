#include "core/rules/parse_support.hpp"

#include <regex>
#include <string>

namespace myclean::rules::detail {

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Location::Location(const std::filesystem::path& file) : where_(utf8(file)) {}

Location::Location(const std::filesystem::path& file, std::string_view kind, std::size_t number)
    : where_(utf8(file) + ": " + std::string(kind) + " #" + std::to_string(number)) {}

void Location::set_id(std::string_view id) {
  where_ += " '" + std::string(id) + "'";
}

void Location::fail(std::string_view field, std::string_view problem) const {
  if (field.empty()) {
    throw SchemaError(where_ + ": " + std::string(problem));
  }
  throw SchemaError(where_ + ": field '" + std::string(field) + "' " + std::string(problem));
}

Pattern compile_pattern(const std::string& text, const Location& location, std::string_view field) {
  try {
    return Pattern{.text = text,
                   .regex = std::regex(text, std::regex::ECMAScript | std::regex::icase)};
  } catch (const std::regex_error& error) {
    location.fail(field, "is not a valid regular expression: " + std::string(error.what()));
  }
}

void claim_id(std::set<std::string, std::less<>>& ids, const std::string& id,
              const Location& location) {
  if (!ids.insert(id).second) {
    location.fail("id", "repeats an id used earlier in the file");
  }
}

}  // namespace myclean::rules::detail
