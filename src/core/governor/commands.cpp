#include "core/governor/commands.hpp"

#include <array>
#include <optional>
#include <string_view>

#include "core/model/enum_names.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 5> step_names = {"priority", "ecoqos", "cap_job", "suspend",
                                                        "kill"};
constexpr std::array<std::string_view, 7> command_names = {
    "apply_step", "roll_back", "persist_suspend", "resume", "kill", "notify", "add_to_allow_list"};

}  // namespace

std::string_view to_string(Step step) {
  return detail::enum_name(step_names, step);
}

std::optional<Step> parse_step(std::string_view text) {
  return detail::enum_from_name<Step>(step_names, text);
}

std::string_view to_string(CommandKind kind) {
  return detail::enum_name(command_names, kind);
}

}  // namespace myclean::governor
