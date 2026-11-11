# The user-scope plans the tray runs under the owner's token: reading a user_tasks.next answer,
# running its native command, disk-cleanup handlers or relocation, and the report record; plus
# the relocation targets as the tray sees them (volumes and disk health it polls); and the owner's
# one-click decisions: a folder moved to the HDD archive, a program's own uninstaller.
myclean_add_library(user_tasks
  SOURCES
    appx.cpp
    appx.hpp
    archive.cpp
    archive.hpp
    drive_facts.cpp
    drive_facts.hpp
    recycle.cpp
    recycle.hpp
    task.cpp
    task.hpp
    uninstall.cpp
    uninstall.hpp
  DEPENDS
    myclean::clean
    myclean::cleanup
    myclean::ipc
    myclean::model
    myclean::os
    myclean::placement
    myclean::safety
    myclean::scanall
    myclean::tray_model
    nlohmann_json::nlohmann_json)
