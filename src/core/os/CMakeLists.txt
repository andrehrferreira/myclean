# Interfaces through which core reaches the operating system, plus the portable SystemClock.
# Win32 implementations live under src/platform; fakes under tests/fakes.
myclean_add_library(os
  SOURCES
    clock.cpp
    clock.hpp
    file_system.hpp
  DEPENDS
    myclean::model)
