#include "core/elimination/suspects.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/classifier.hpp"
#include "core/input/incident.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::elimination {
namespace {

constexpr std::array<std::string_view, 2> mode_names = {"suspend", "guided"};

// The suspects known to own a keyboard hook (analysis F-029, mechanism d): PowerToys Keyboard
// Manager's low-level keyboard hook, and the global hotkeys and hooks of Armoury Crate and
// Discord.
constexpr std::array<std::string_view, 3> hook_owners = {
    "PowerToys.KeyboardManagerEngine.exe", "ArmouryCrate.UserSessionHelper.exe", "Discord.exe"};

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string_view to_string(PauseMode mode) {
  return detail::enum_name(mode_names, mode);
}

std::optional<PauseMode> parse_pause_mode(std::string_view text) {
  return detail::enum_from_name<PauseMode>(mode_names, text);
}

bool same_image(std::string_view image, std::string_view suspect) {
  return std::ranges::equal(image, suspect, {}, lower, lower);
}

std::vector<Suspect> ranked_suspects(std::span<const input::StoredIncident> incidents) {
  std::vector<Suspect> suspects;
  std::int64_t rank = 0;
  for (const std::string_view image : input::known_suspects()) {
    const bool known_hook = std::ranges::any_of(
        hook_owners, [&](std::string_view owner) { return same_image(image, owner); });
    const bool seen_with_hook =
        std::ranges::any_of(incidents, [&](const input::StoredIncident& stored) {
          return stored.classification.mechanism == input::Mechanism::slow_keyboard_hook &&
                 std::ranges::any_of(stored.incident.suspects, [&](const std::string& running) {
                   return same_image(running, image);
                 });
        });
    suspects.push_back(
        Suspect{.id = ++rank, .image = std::string(image), .hook = known_hook || seen_with_hook});
  }
  return suspects;
}

std::optional<Suspect> suspect_by_id(std::span<const input::StoredIncident> incidents,
                                     std::int64_t id) {
  std::vector<Suspect> suspects = ranked_suspects(incidents);
  const auto found = std::ranges::find(suspects, id, &Suspect::id);
  if (found == suspects.end()) {
    return std::nullopt;
  }
  return *found;
}

}  // namespace myclean::elimination
