myclean_add_test(sched
  SOURCES
    cron_test.cpp
    scheduler_test.cpp
    store_schedule_repository_test.cpp
  DEPENDS
    myclean::sched
    myclean::fakes)
