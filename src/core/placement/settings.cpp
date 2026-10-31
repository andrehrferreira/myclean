#include "core/placement/settings.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean::placement {

namespace {

constexpr std::array<std::string_view, 3> hibernation_mode_names = {"off", "reduced", "full"};

}  // namespace

std::string_view to_string(HibernationMode mode) {
  return detail::enum_name(hibernation_mode_names, mode);
}

std::optional<HibernationMode> parse_hibernation_mode(std::string_view text) {
  return detail::enum_from_name<HibernationMode>(hibernation_mode_names, text);
}

std::string Command::text() const {
  std::string result = tool;
  if (result.ends_with(".exe")) {
    result.resize(result.size() - 4);
  }
  for (const std::string& argument : arguments) {
    result += ' ';
    result += argument;
  }
  return result;
}

}  // namespace myclean::placement
