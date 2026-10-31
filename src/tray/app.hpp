#pragma once

#include <string_view>

#include <windows.h>

#include "tray/instance.hpp"

namespace myclean::tray {

// Exit codes of the tray.
inline constexpr int exit_ok = 0;
inline constexpr int exit_start_failed = 1;

// The tray's whole life on the UI thread: a hidden window for the icon's messages, the icon,
// the poll thread on `pipe_name` and, on request, the dashboard. Returns when the owner chooses
// Quit or `instance`'s quit event is signaled, after the icon was removed and the poll thread
// joined.
[[nodiscard]] int run(HINSTANCE module, const Instance& instance, std::wstring_view pipe_name);

}  // namespace myclean::tray
