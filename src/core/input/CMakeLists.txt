# The Ctrl+C / Ctrl+V freeze watchdog: the raw keyboard chord filter, the copy-latency and
# paste-stall state machine over an injected probe, the mechanism classifier, and the
# input_incident table with the Freezes-view summary. Win32 probes live in src/platform/input.
myclean_add_library(input
  SOURCES
    chord_filter.cpp
    chord_filter.hpp
    classifier.cpp
    classifier.hpp
    incident.cpp
    incident.hpp
    incident_store.cpp
    incident_store.hpp
    probe.hpp
    watchdog.cpp
    watchdog.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::store
    nlohmann_json::nlohmann_json)
