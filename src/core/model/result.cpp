#include "core/model/result.hpp"

#include <array>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean {

namespace {

constexpr std::array<std::string_view, 11> errc_names = {
    "invalid_argument", "invalid_data",   "invalid_state", "not_found",
    "already_exists",   "not_empty",      "refused",       "access_denied",
    "in_use",           "not_reversible", "io_error",
};

}  // namespace

std::string_view to_string(Errc code) {
  return detail::enum_name(errc_names, code);
}

}  // namespace myclean
