#pragma once

#include <optional>
#include <string_view>

#include "core/governor/policy.hpp"

// Game mode (governor spec, "Game Mode"): whether it is active and the CPU cap of build jobs.
// Protection does not depend on it: a detected game is class `game` (final_class) and receives no
// action in every override state.
namespace myclean::governor {

// The tray's override. Stored as text: "auto", "on", "off".
enum class GameModeOverride { automatic, on, off };

[[nodiscard]] std::string_view to_string(GameModeOverride value);
[[nodiscard]] std::optional<GameModeOverride> parse_game_mode_override(std::string_view text);

struct GameMode {
  bool active = false;         // the stricter ladder (PressurePolicy::game_dwell, game_cooldown)
  int build_cpu_percent = 75;  // CPU cap of every build job
};

// `on` and `off` decide alone; `auto` follows game detection. Active: the build cap is at most
// JobPolicy::game_build_cpu_percent; otherwise it is the build class's cap.
[[nodiscard]] GameMode decide_game_mode(GameModeOverride override_value, bool game_running,
                                        const JobPolicy& policy);

}  // namespace myclean::governor
