#pragma once

#include <charconv>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/mcp/client.hpp"
#include "core/model/result.hpp"

// What the commands of myclean.exe share: the parsed command line, how a reply is printed (the
// service's JSON unchanged with --json, else the command's text) and the text helpers.
namespace myclean::cli {

// A parsed command line: the command words, the flags, and the options that take a value.
struct CommandLine {
  std::vector<std::string> words;
  bool json = false;
  bool refresh = false;                                      // --refresh
  std::vector<std::pair<std::string, std::string>> options;  // name without "--", value
};

// The value of option `name`, or null.
[[nodiscard]] const std::string* find_option(const CommandLine& line, std::string_view name);

// Option `name` as a number; nullopt when it is not given, an error when it is not a number.
template <typename T>
[[nodiscard]] Result<std::optional<T>> option(const CommandLine& line, std::string_view name) {
  const std::string* text = find_option(line, name);
  if (text == nullptr) {
    return std::optional<T>{};
  }
  const char* const last = std::to_address(text->end());
  T value{};
  const auto [end, error] = std::from_chars(text->data(), last, value);
  if (error != std::errc{} || end != last) {
    return fail(Errc::invalid_argument, "--" + std::string(name) + ": not a number: " + *text);
  }
  return std::optional<T>{value};
}

[[nodiscard]] std::string dump(const nlohmann::json& value);

// Prints a failed call and returns exit_unavailable.
int report(const mcp::CallError& error, bool json, std::ostream& out, std::ostream& err);

// Prints `reply` as JSON or as `render` makes it, or its error; returns a cli exit code.
int show(const mcp::Reply& reply, bool json, std::ostream& out, std::ostream& err,
         std::string (*render)(const nlohmann::json&));

// Text helpers: a value the service did not report prints as "n/a".
namespace render {

[[nodiscard]] std::optional<double> number(const nlohmann::json& object, std::string_view key);
[[nodiscard]] std::string text_of(const nlohmann::json& object, std::string_view key);
[[nodiscard]] std::string bytes(std::optional<double> value);  // "1.5 GiB"
[[nodiscard]] std::string fixed(std::optional<double> value, int decimals = 1);
[[nodiscard]] std::string utc(const nlohmann::json& object, std::string_view key);  // Unix seconds
// Member `key` of `object` when it is an array, else an empty array.
[[nodiscard]] nlohmann::json list_of(const nlohmann::json& object, std::string_view key);

}  // namespace render
}  // namespace myclean::cli
