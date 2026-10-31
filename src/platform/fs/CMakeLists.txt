# Win32 implementation of core/os/file_system.hpp, and the deny-list locations of this machine.
myclean_add_library(platform_fs
  SOURCES
    protected_locations.cpp
    protected_locations.hpp
    win32_file_system.cpp
    win32_file_system.hpp
  DEPENDS
    myclean::os
    myclean::safety
    WIL::WIL
    shell32
    ole32
    uuid)
