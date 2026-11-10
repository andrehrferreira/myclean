# core/vhdx on Windows: compaction through the Virtual Disk API and `wsl.exe --shutdown` as the
# owner (through platform_git's hidden process runner).
myclean_add_library(platform_vhdx
  SOURCES
    win32_vhdx.cpp
    win32_vhdx.hpp
  DEPENDS
    myclean::vhdx
    myclean::platform_git
    WIL::WIL
    virtdisk)
