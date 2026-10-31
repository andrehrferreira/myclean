# Win32 side of cleaning through Windows and the tools themselves (core/cleanup): the
# disk-cleanup handler host (IEmptyVolumeCache on a private desktop) and the command runner
# (hidden child processes in a job object).
myclean_add_library(platform_cleanup
  SOURCES
    wide.hpp
    win32_command_runner.cpp
    win32_command_runner.hpp
    win32_handler_host.cpp
    win32_handler_host.hpp
  DEPENDS
    myclean::cleanup
    myclean::platform_ipc
    WIL::WIL
    advapi32
    ole32
    user32)
