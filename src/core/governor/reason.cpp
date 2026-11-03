#include "core/governor/reason.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 17> reason_names = {
    "cpu",         "available_memory", "commit",
    "hard_faults", "pressure_cleared", "joined_tree",
    "protected",   "allow_once",       "always_allow",
    "resume",      "owner_kill",       "pause",
    "stop",        "orphaned_suspend", "untrusted_job",
    "policy",      "orphaned_loop"};

}  // namespace

std::string_view to_string(Reason reason) {
  return detail::enum_name(reason_names, reason);
}

std::optional<Reason> parse_reason(std::string_view text) {
  return detail::enum_from_name<Reason>(reason_names, text);
}

bool is_pressure_signal(Reason reason) {
  return reason == Reason::cpu || reason == Reason::available_memory || reason == Reason::commit ||
         reason == Reason::hard_faults;
}

}  // namespace myclean::governor
