#include "core/model/finding.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean {

namespace {

constexpr std::array<std::string_view, 3> severity_names = {"info", "warning", "critical"};

}  // namespace

std::string_view to_string(Severity severity) {
  return detail::enum_name(severity_names, severity);
}

std::optional<Severity> parse_severity(std::string_view text) {
  return detail::enum_from_name<Severity>(severity_names, text);
}

}  // namespace myclean
