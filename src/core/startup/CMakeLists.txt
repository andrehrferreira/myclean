# Startup items: the inventory of every automatic start source (Run and RunOnce keys in both
# registry views, Startup folders, logon and boot tasks, third-party auto-start services) with
# its StartupApproved state, and the StartupToggle action that disables and enables them the way
# Windows does, with exact undo; and the measured impact of each program in the first 2 minutes
# after a logon (table logon_impact). The Win32 reader and writer live in src/platform/startup.
myclean_add_library(startup
  SOURCES
    impact.cpp
    impact.hpp
    startup.cpp
    startup.hpp
    toggle.cpp
    toggle.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::store
    nlohmann_json::nlohmann_json)
