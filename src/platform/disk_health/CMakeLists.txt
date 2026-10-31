# Win32 disk-health reader (core/health/source.hpp): storage IOCTLs, SMART, SAT pass-through and
# the WMI reliability counters.
myclean_add_library(platform_disk_health
  SOURCES
    reliability_counters.cpp
    win32_disk_health.cpp
    win32_disk_health.hpp
  DEPENDS
    myclean::health
    WIL::WIL
    ole32
    oleaut32
    propsys
    wbemuuid)
