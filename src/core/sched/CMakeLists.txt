# MyClean's in-app scheduler: cron parser, persisted schedule state, catch-up. No OS scheduler.
myclean_add_library(sched
  SOURCES
    cron.cpp
    cron.hpp
    schedule_repository.cpp
    schedule_repository.hpp
    scheduler.cpp
    scheduler.hpp
    store_schedule_repository.cpp
    store_schedule_repository.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::store)
