# Interfaces through which core reaches the operating system, plus the portable SystemClock.
# Win32 implementations live under src/platform; fakes under tests/fakes.
myclean_add_library(os
  SOURCES
    clock.cpp
    clock.hpp
    file_system.hpp
    jobs.cpp
    jobs.hpp
    process_control.hpp
  DEPENDS
    myclean::model)
