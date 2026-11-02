# Win32 side of cleaning through Windows and the tools themselves (core/cleanup): the
# disk-cleanup handler host (IEmptyVolumeCache on a private desktop), the command runner (hidden
# child processes in a job object), the owner's $I records and the shell's Recycle Bin folder
# (IFileOperation with FOF_NO_UI on a private desktop).
myclean_add_library(platform_cleanup
  SOURCES
    wide.hpp
    win32_command_runner.cpp
    win32_command_runner.hpp
    win32_handler_host.cpp
    win32_handler_host.hpp
    win32_recycle_bin.cpp
    win32_recycle_bin.hpp
  DEPENDS
    myclean::cleanup
    myclean::platform_ipc
    WIL::WIL
    advapi32
    ole32
    propsys
    shell32
    user32
    uuid)
