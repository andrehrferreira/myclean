# Startup items: the inventory of every automatic start source (Run and RunOnce keys in both
# registry views, Startup folders, logon and boot tasks, third-party auto-start services) with
# its StartupApproved state, and the StartupToggle action that disables and enables them the way
# Windows does, with exact undo. The Win32 reader and writer live in src/platform/startup.
myclean_add_library(startup
  SOURCES
    startup.cpp
    startup.hpp
    toggle.cpp
    toggle.hpp
  DEPENDS
    myclean::model
    myclean::os
    nlohmann_json::nlohmann_json)
