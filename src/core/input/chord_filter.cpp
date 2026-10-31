#include "core/input/chord_filter.hpp"

#include <cstdint>
#include <optional>

#include "core/input/incident.hpp"

namespace myclean::input {

namespace {

// Virtual-key codes (winuser.h). Raw input reports Ctrl and Alt as the generic codes, left and
// right told apart by the E0 prefix; the sided codes are accepted as well.
constexpr std::uint16_t vk_control = 0x11;
constexpr std::uint16_t vk_menu = 0x12;
constexpr std::uint16_t vk_c = 0x43;
constexpr std::uint16_t vk_v = 0x56;
constexpr std::uint16_t vk_lcontrol = 0xA2;
constexpr std::uint16_t vk_rcontrol = 0xA3;
constexpr std::uint16_t vk_lmenu = 0xA4;
constexpr std::uint16_t vk_rmenu = 0xA5;

}  // namespace

std::optional<Chord> ChordFilter::on_key(std::uint16_t virtual_key, bool extended, bool released) {
  const bool down = !released;
  switch (virtual_key) {
    case vk_control:
      (extended ? right_ctrl_ : left_ctrl_) = down;
      return std::nullopt;
    case vk_lcontrol:
      left_ctrl_ = down;
      return std::nullopt;
    case vk_rcontrol:
      right_ctrl_ = down;
      return std::nullopt;
    case vk_menu:
      (extended ? right_alt_ : left_alt_) = down;
      return std::nullopt;
    case vk_lmenu:
      left_alt_ = down;
      return std::nullopt;
    case vk_rmenu:
      right_alt_ = down;
      return std::nullopt;
    case vk_c:
    case vk_v: {
      bool& held = virtual_key == vk_c ? c_down_ : v_down_;
      const bool repeat = held && down;
      held = down;
      const bool ctrl = left_ctrl_ || right_ctrl_;
      const bool alt = left_alt_ || right_alt_;
      if (!down || repeat || !ctrl || alt) {
        return std::nullopt;
      }
      return virtual_key == vk_c ? Chord::copy : Chord::paste;
    }
    default:
      return std::nullopt;
  }
}

}  // namespace myclean::input
