# Win32 readers for drive maintenance (core/drives), all read-only: file cluster runs and the
# volume bitmap, the storage optimizer's event log, TRIM support, the power source, and the
# background priority the sampling runs at.
myclean_add_library(platform_defrag
  SOURCES
    system_sources.cpp
    system_sources.hpp
    volume_layout.cpp
    volume_layout.hpp
    win32_support.hpp
  DEPENDS
    myclean::drives
    WIL::WIL
    advapi32
    wevtapi)
