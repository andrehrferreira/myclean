# Guided freeze elimination (input-watchdog spec I3): the ranked copy/paste freeze suspects, the
# baseline incident rate, the time-boxed pauses of one suspect at a time (suspended, or quit by the
# owner for a suspect that owns a keyboard hook), their automatic restore and the incident rates
# compared, and the PauseSuspect action the owner approves in the tray.
myclean_add_library(elimination
  SOURCES
    keeper.cpp
    keeper.hpp
    pause_action.cpp
    pause_action.hpp
    pauses.cpp
    pauses.hpp
    suspects.cpp
    suspects.hpp
    views.cpp
    views.hpp
  DEPENDS
    myclean::actions
    myclean::alerts
    myclean::health
    myclean::input
    myclean::model
    myclean::os
    myclean::store
    myclean::telemetry
    nlohmann_json::nlohmann_json)
