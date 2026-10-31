# "Where did my disk go": data/attribution.toml, its loader, the map of a scan (what each place
# is and how it can be reclaimed) and its JSON form. Read-only.
myclean_add_library(map
  SOURCES
    attribution.cpp
    attribution.hpp
    disk_map.cpp
    disk_map.hpp
    json.cpp
    json.hpp
  DEPENDS
    myclean::clean
    myclean::rules
    myclean::space
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus)
