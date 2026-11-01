#include "core/governor/game_mode.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "core/governor/policy.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 3> override_names = {"auto", "on", "off"};

}  // namespace

std::string_view to_string(GameModeOverride value) {
  return detail::enum_name(override_names, value);
}

std::optional<GameModeOverride> parse_game_mode_override(std::string_view text) {
  return detail::enum_from_name<GameModeOverride>(override_names, text);
}

GameMode decide_game_mode(GameModeOverride override_value, bool game_running,
                          const JobPolicy& policy) {
  const bool active = override_value == GameModeOverride::on ||
                      (override_value == GameModeOverride::automatic && game_running);
  return {.active = active,
          .build_cpu_percent =
              active ? std::min(policy.build.cpu_percent, policy.game_build_cpu_percent)
                     : policy.build.cpu_percent};
}

}  // namespace myclean::governor
