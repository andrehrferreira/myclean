# `myclean scan-all` analyses (scan-all spec): cold data, placement advice, cross-drive
# duplicates, the findings of the map, rules, compression and maintenance analyses, and the
# unified report with its precedence rule. Read-only; the CLI does the I/O.
myclean_add_library(scanall
  SOURCES
    cold.cpp
    cold.hpp
    drive_health.cpp
    drive_health.hpp
    duplicates.cpp
    duplicates.hpp
    findings.cpp
    findings.hpp
    placement.cpp
    placement.hpp
    report.cpp
    report.hpp
    sources.cpp
    sources.hpp
  DEPENDS
    myclean::clean
    myclean::compress
    myclean::drives
    myclean::health
    myclean::ipc
    myclean::map
    myclean::placement
    myclean::rules
    myclean::space
    nlohmann_json::nlohmann_json)
