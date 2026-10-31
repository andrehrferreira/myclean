# Win32 process sources: the process table (core/telemetry/process_table.hpp).
myclean_add_library(platform_proc
  SOURCES
    nt_process_table.cpp
    nt_process_table.hpp
  DEPENDS
    myclean::telemetry)
