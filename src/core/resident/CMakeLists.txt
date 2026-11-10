# Resident processes (optimizer spec, "Memory-Leak Detection", "Background-App Cost Report"): the
# use of every process tree (CPU time, private bytes, RAM, disk and network bytes) from the
# telemetry's process tables, summed per program and minute and recorded in tables resident_fine
# (5-minute private bytes, 2 days) and resident_hour (hourly totals, 35 days); the 30-day cost
# report and the leak detector, which only reports to the alert engine and never acts on a
# process.
myclean_add_library(resident
  SOURCES
    history.cpp
    history.hpp
    leak.cpp
    leak.hpp
    sampler.cpp
    sampler.hpp
    tracker.cpp
    tracker.hpp
  DEPENDS
    myclean::alerts
    myclean::model
    myclean::store
    myclean::telemetry)
