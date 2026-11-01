# The resource governor in the service (governor spec, .rulebook/tasks/phase3_resource-governor):
# the runtime that applies core/governor's decisions (proactive build/test jobs, the ladder, game
# mode, crash recovery and fail-open) over the core/os job and process interfaces, its tables
# (governor_event, governor_job, governor_suspend, governor_allow; GovernorStep in action_log),
# observe/act mode, and the methods of core/ipc/governor.hpp.
myclean_add_library(governing
  SOURCES
    executor.cpp
    executor.hpp
    governor.cpp
    governor.hpp
    job_book.cpp
    job_book.hpp
    keys.hpp
    methods.cpp
    methods.hpp
    records.cpp
    records.hpp
  DEPENDS
    myclean::actions
    myclean::events
    myclean::games
    myclean::governor
    myclean::ipc
    myclean::model
    myclean::os
    myclean::rules
    myclean::store
    myclean::telemetry
    nlohmann_json::nlohmann_json
    spdlog::spdlog)
