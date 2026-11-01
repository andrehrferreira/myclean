# Win32 side of the governor's job objects and per-process actions (core/os/jobs.hpp,
# core/os/process_control.hpp). ntdll functions are resolved at run time, never imported.
myclean_add_library(platform_jobs
  SOURCES
    jobs_support.cpp
    jobs_support.hpp
    win32_jobs.cpp
    win32_jobs.hpp
    win32_process_control.cpp
    win32_process_control.hpp
  DEPENDS
    myclean::os
    WIL::WIL)
