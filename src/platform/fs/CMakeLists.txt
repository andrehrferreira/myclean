# Win32 implementation of core/os/file_system.hpp, the deny-list locations of this machine, and
# the guarded delete of MyClean's own files.
myclean_add_library(platform_fs
  SOURCES
    own_files.cpp
    own_files.hpp
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
