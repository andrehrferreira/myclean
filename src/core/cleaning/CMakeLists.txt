# Cleaning in the service (C4, C5, R8 service side): the executor that turns rule candidates into
# plans by tier and scope and runs the approved ones, the user tasks of the tray, the recurring
# auto run and quarantine purge, the space analyses, and the methods of core/ipc/plans.hpp.
myclean_add_library(cleaning
  SOURCES
    executor.cpp
    executor.hpp
    progress.cpp
    progress.hpp
    records.cpp
    records.hpp
    service.cpp
    service.hpp
    tray_tasks.cpp
    tray_tasks.hpp
    views.cpp
    views.hpp
  DEPENDS
    myclean::actions
    myclean::clean
    myclean::cleanup
    myclean::ipc
    myclean::packs
    myclean::placement
    myclean::rules
    myclean::scanall
    myclean::sched
    myclean::space
    myclean::startup
    myclean::store
    nlohmann_json::nlohmann_json)
