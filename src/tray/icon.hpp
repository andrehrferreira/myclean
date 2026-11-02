#pragma once

#include <span>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/tray_model/tray_state.hpp"

namespace myclean::tray {

// A 32-bit icon with alpha from `size` x `size` pixels (tray_model::icon_pixels). Creating it
// shows nothing; the tray icon displays it.
[[nodiscard]] Result<wil::unique_hicon> make_icon(std::span<const tray_model::Argb> pixels,
                                                  int size);

// The pixels of icon resource 1 of `module` (the application icon) at `size` x `size`, row by row
// from the top (the base of tray_model::icon_pixels). Fails when the module has no such icon or
// it has no alpha channel.
[[nodiscard]] Result<std::vector<tray_model::Argb>> app_icon_pixels(HINSTANCE module, int size);

}  // namespace myclean::tray
