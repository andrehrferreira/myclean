# Disk health: interpretation of NVMe health logs, ATA SMART data and reliability counters,
# drive status, delta and temperature alerts, endurance projection, daily history in the store,
# and the 10-min collector. The Win32 reader lives in src/platform/disk_health.
myclean_add_library(health
  SOURCES
    analysis.cpp
    analysis.hpp
    collector.cpp
    collector.hpp
    drive_specs.cpp
    drive_specs.hpp
    history.cpp
    history.hpp
    reading.cpp
    reading.hpp
    source.hpp
  DEPENDS
    myclean::alerts
    myclean::model
    myclean::os
    myclean::rules
    myclean::store
    myclean::telemetry
    nlohmann_json::nlohmann_json)
