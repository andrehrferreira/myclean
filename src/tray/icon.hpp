#pragma once

#include <span>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray {

// A 32-bit icon with alpha from `size` x `size` pixels (tray_model::icon_pixels). Creating it
// shows nothing; the tray icon displays it.
[[nodiscard]] Result<wil::unique_hicon> make_icon(std::span<const tray_model::Argb> pixels,
                                                  int size);

}  // namespace myclean::tray
