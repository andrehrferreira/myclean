# Win32 performance sources: PDH counter queries (core/telemetry/perf_counters.hpp) and the
# memory status (core/telemetry/memory_status.hpp).
myclean_add_library(platform_perf
  SOURCES
    pdh_counters.cpp
    pdh_counters.hpp
    win32_memory_status.cpp
    win32_memory_status.hpp
  DEPENDS
    myclean::telemetry
    pdh)
