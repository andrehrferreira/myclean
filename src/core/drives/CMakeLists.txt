# Drive maintenance: per-volume analysis (SSD TRIM support and last retrim from the optimizer
# log, HDD fragmentation sampled from file cluster runs, free-space layout from the volume
# bitmap), recommendations, and the RetrimVolume / DefragVolume actions gated on health, idle
# disks and mains power. The Win32 readers live in src/platform/defrag.
myclean_add_library(drives
  SOURCES
    actions.cpp
    actions.hpp
    analysis.cpp
    analysis.hpp
    history.cpp
    history.hpp
    layout.cpp
    layout.hpp
  DEPENDS
    myclean::health
    myclean::model
    myclean::os
    myclean::placement
    myclean::space
    myclean::store
    myclean::telemetry
    nlohmann_json::nlohmann_json)
