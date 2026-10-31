#pragma once

#include <cstdint>
#include <optional>

#include "core/input/incident.hpp"

namespace myclean::input {

// The raw keyboard filter: the only code that sees every keystroke. It keeps the state of Ctrl,
// Alt, C and V and nothing else, and turns key events into Ctrl+C / Ctrl+V chords. Every other
// key is dropped on entry, so no buffer or log can ever hold it.
class ChordFilter {
 public:
  // One raw keyboard event: the virtual-key code, whether it has the E0 prefix (right Ctrl or
  // right Alt), and whether the key was released. Returns the chord on the keydown of C or V
  // while Ctrl is held and Alt is not (Ctrl+Alt is AltGr); the auto-repeat of a held C or V
  // returns nothing.
  [[nodiscard]] std::optional<Chord> on_key(std::uint16_t virtual_key, bool extended,
                                            bool released);

 private:
  bool left_ctrl_ = false;
  bool right_ctrl_ = false;
  bool left_alt_ = false;
  bool right_alt_ = false;
  bool c_down_ = false;
  bool v_down_ = false;
};

}  // namespace myclean::input
