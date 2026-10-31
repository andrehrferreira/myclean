# Telemetry: the interfaces to the OS sources (process table, performance counters, memory
# status, volumes), the collectors built on them, the sampling pipeline into the store, and
# history queries. Win32 sources live in src/platform/proc, perf and disk.
myclean_add_library(telemetry
  SOURCES
    collector.hpp
    gpu_collector.cpp
    gpu_collector.hpp
    history.cpp
    history.hpp
    memory_status.hpp
    metrics.hpp
    perf_counters.hpp
    pipeline.cpp
    pipeline.hpp
    process_collector.cpp
    process_collector.hpp
    process_table.hpp
    registration.cpp
    registration.hpp
    system_collectors.cpp
    system_collectors.hpp
    volume_collector.cpp
    volume_collector.hpp
    volume_info.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::store)
