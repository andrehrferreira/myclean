# Win32 disk sources: mounted volumes and their physical disks
# (core/telemetry/volume_info.hpp).
myclean_add_library(platform_disk
  SOURCES
    win32_volume_info.cpp
    win32_volume_info.hpp
  DEPENDS
    myclean::telemetry
    WIL::WIL)
