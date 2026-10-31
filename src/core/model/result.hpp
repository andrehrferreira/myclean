#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

namespace myclean {

// Error categories shared by every core module. The message carries the specifics.
enum class Errc {
  invalid_argument,  // a caller passed a malformed value (bad cron field, relative path)
  invalid_data,      // stored or loaded data failed validation (rule schema, integrity check)
  invalid_state,     // not allowed in the current state (illegal plan transition)
  not_found,
  already_exists,
  not_empty,       // a directory that must be empty is not
  refused,         // refused by MyClean's own safety rules or policy (deny-list, approval rights)
  access_denied,   // refused by the operating system
  in_use,          // the OS reports a sharing violation or lock
  not_reversible,  // an undo was requested for a change that cannot be undone
  io_error,        // any other operating-system or storage failure
};

[[nodiscard]] std::string_view to_string(Errc code);

struct Error {
  Errc code;
  std::string message;
};

// The return type of every fallible core operation. Result<void> carries no value.
template <typename T>
using Result = tl::expected<T, Error>;

// Builds the error branch of a Result: `return fail(Errc::not_found, "no such plan");`
[[nodiscard]] inline tl::unexpected<Error> fail(Errc code, std::string message) {
  return tl::unexpected<Error>(Error{.code = code, .message = std::move(message)});
}

}  // namespace myclean
