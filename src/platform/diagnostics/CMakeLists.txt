# Win32 reader of Windows' boot measurements (core/startup/diagnostics.hpp): the events 100 and
# 101 of Microsoft-Windows-Diagnostics-Performance/Operational, read-only, through EvtQuery.
myclean_add_library(platform_diagnostics
  SOURCES
    win32_diagnostics_log.cpp
    win32_diagnostics_log.hpp
  DEPENDS
    myclean::startup
    myclean::platform_settings
    WIL::WIL
    wevtapi)
