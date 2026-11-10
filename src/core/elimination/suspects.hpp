#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/incident_store.hpp"

// The copy/paste freeze suspects guided elimination pauses (input-watchdog spec, "Guided
// Elimination Offer"), in the rank of input::known_suspects() (analysis F-029).
namespace myclean::elimination {

// How a suspect is paused.
enum class PauseMode {
  // Every process of the suspect in the owner's session is suspended and resumed at the end.
  suspend,
  // The suspect owns a low-level keyboard or mouse hook: suspended, it would make every keystroke
  // wait for the hook timeout until Windows silently drops the hook, and the program would stay
  // broken after the resume. The owner quits it instead; MyClean counts only the hours no process
  // of it runs, reminds the owner to start it again and never launches or ends it.
  guided,
};

[[nodiscard]] std::string_view to_string(PauseMode mode);
[[nodiscard]] std::optional<PauseMode> parse_pause_mode(std::string_view text);

struct Suspect {
  std::int64_t id = 0;  // 1-based rank in input::known_suspects()
  std::string image;
  // Owns a keyboard hook: known to (PowerToys Keyboard Manager, Armoury Crate's session helper,
  // Discord), or running in an incident the watchdog classified as a slow keyboard hook.
  bool hook = false;

  [[nodiscard]] PauseMode mode() const { return hook ? PauseMode::guided : PauseMode::suspend; }

  friend bool operator==(const Suspect&, const Suspect&) = default;
};

// Every known suspect in rank order, with its hook judged from `incidents` as well.
[[nodiscard]] std::vector<Suspect> ranked_suspects(
    std::span<const input::StoredIncident> incidents);
// The suspect of rank `id`; nullopt for an id outside the list.
[[nodiscard]] std::optional<Suspect> suspect_by_id(std::span<const input::StoredIncident> incidents,
                                                   std::int64_t id);

// Whether `image` names suspect `suspect` (file names compared without case).
[[nodiscard]] bool same_image(std::string_view image, std::string_view suspect);

}  // namespace myclean::elimination
