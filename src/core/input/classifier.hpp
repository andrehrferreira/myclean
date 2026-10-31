#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/incident.hpp"

namespace myclean::input {

// The ways copy/paste can block (analysis F-026), plus unexplained.
enum class Mechanism {
  delayed_rendering,
  clipboard_held_open,
  viewer_chain,
  slow_keyboard_hook,
  heavy_payload,
  unexplained,
};

[[nodiscard]] std::string_view to_string(Mechanism mechanism);
[[nodiscard]] std::optional<Mechanism> parse_mechanism(std::string_view text);

struct Classification {
  Mechanism mechanism = Mechanism::unexplained;
  // The process named as the cause; none for slow keyboard hook (hook owners cannot be listed)
  // and for unexplained.
  std::optional<ProcessRef> process;
  // One sentence per fact the mechanism was chosen on.
  std::vector<std::string> evidence;

  friend bool operator==(const Classification&, const Classification&) = default;
};

// The first fingerprint that matches, in this order:
//   delayed rendering    paste, and the clipboard owner (another thread) failed a WM_NULL probe
//   clipboard held open  another process held the clipboard open during the stall
//   viewer chain         copy, and the legacy viewer chain is not empty
//   slow keyboard hook   the key's input lag exceeds incident_threshold (provisional: owner test
//                        T2 decides whether the WM_INPUT message time is the input event time)
//   heavy payload        copy, and a heavy format (is_heavy) was set, not synthesized
// and unexplained otherwise. Hung windows (IsHungAppWindow) are never a fingerprint.
[[nodiscard]] Classification classify(const Incident& incident);

}  // namespace myclean::input
