# The Dev Drive advisor (placement spec): what a Dev Drive is, the active repositories and
# package caches that would move onto one, and the owner's steps. Advice only; the Win32 probe
# lives in src/platform/devdrive.
myclean_add_library(devdrive
  SOURCES
    advisor.cpp
    advisor.hpp
  DEPENDS
    myclean::clean
    myclean::map
    myclean::placement
    nlohmann_json::nlohmann_json)
