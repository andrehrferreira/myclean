# Win32 probes of the cleaning rule engine (core/clean/probes.hpp): file tree with times,
# environment, file locks and process folders.
myclean_add_library(platform_clean
  SOURCES
    win32_probes.cpp
    win32_probes.hpp
  DEPENDS
    myclean::clean
    myclean::platform_games
    WIL::WIL)
