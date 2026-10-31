# Win32 side of the input watchdog: the user32 probes (core/input/probe.hpp) and the tray-hosted
# InputWatchdog with its message-only window, raw keyboard sink and clipboard listener.
myclean_add_library(platform_input
  SOURCES
    input_watchdog.cpp
    input_watchdog.hpp
    win32_input_probe.cpp
    win32_input_probe.hpp
  DEPENDS
    myclean::input
    WIL::WIL
    user32)
