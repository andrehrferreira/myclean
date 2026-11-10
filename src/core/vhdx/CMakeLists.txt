# WSL and Docker virtual disks (optimizer spec C6): the size report from the knowledge packs'
# locations and the ask-tier CompactVhdx action. The Win32 side lives in src/platform/vhdx.
myclean_add_library(vhdx
  SOURCES
    compact.cpp
    compact.hpp
    report.cpp
    report.hpp
  DEPENDS
    myclean::clean
    myclean::map
    myclean::model
    myclean::os
    nlohmann_json::nlohmann_json)
