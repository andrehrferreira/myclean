#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/replies.hpp"

// Reading the members of a service answer for the tray's screens. A member the contract makes
// optional or nullable may be absent or null; a required one that is missing throws, and
// `parsed` turns that into invalid_data of the method.
namespace myclean::tray_model::json_read {

using Json = nlohmann::json;

// The member `name`, or null when it is absent.
[[nodiscard]] inline const Json& member(const Json& object, const char* name) {
  static const Json absent;
  const auto found = object.find(name);
  return found == object.end() ? absent : *found;
}

[[nodiscard]] inline std::string text(const Json& object, const char* name) {
  const Json& value = member(object, name);
  return value.is_string() ? value.get<std::string>() : std::string();
}

[[nodiscard]] inline std::optional<std::string> optional_text(const Json& object,
                                                              const char* name) {
  const Json& value = member(object, name);
  return value.is_string() ? std::optional(value.get<std::string>()) : std::nullopt;
}

[[nodiscard]] inline std::optional<std::int64_t> optional_int(const Json& object,
                                                              const char* name) {
  const Json& value = member(object, name);
  return value.is_number() ? std::optional(value.get<std::int64_t>()) : std::nullopt;
}

[[nodiscard]] inline std::optional<double> optional_number(const Json& object, const char* name) {
  const Json& value = member(object, name);
  return value.is_number() ? std::optional(value.get<double>()) : std::nullopt;
}

[[nodiscard]] inline std::optional<bool> optional_bool(const Json& object, const char* name) {
  const Json& value = member(object, name);
  return value.is_boolean() ? std::optional(value.get<bool>()) : std::nullopt;
}

[[nodiscard]] inline std::optional<WallTime> optional_time(const Json& object, const char* name) {
  const std::optional<std::int64_t> seconds = optional_int(object, name);
  return seconds ? std::optional(from_unix_seconds(*seconds)) : std::nullopt;
}

[[nodiscard]] inline Bytes bytes(const Json& object, const char* name) {
  const Json& value = member(object, name);
  return value.is_number() ? Bytes{value.get<std::uint64_t>()} : Bytes{};
}

// The strings of the array `name`; empty when it is absent.
[[nodiscard]] inline std::vector<std::string> texts(const Json& object, const char* name) {
  const Json& value = member(object, name);
  return value.is_array() ? value.get<std::vector<std::string>>() : std::vector<std::string>{};
}

// The array `name`, or an empty one when it is absent or null.
[[nodiscard]] inline const Json& array(const Json& object, const char* name) {
  static const Json empty = Json::array();
  const Json& value = member(object, name);
  return value.is_array() ? value : empty;
}

// The result of `parse`, or the exception it threw as invalid data of `method`.
template <typename Parse>
[[nodiscard]] auto parsed(std::string_view method, const Parse& parse)
    -> Result<decltype(parse())> {
  try {
    return parse();
  } catch (const std::exception& error) {
    return fail(Errc::invalid_data, std::string(method) + ": " + error.what());
  }
}

}  // namespace myclean::tray_model::json_read
