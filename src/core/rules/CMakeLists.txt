# Schemas and loaders of the data files: data/rules/*.toml, data/classes.toml, data/kb/*.json.
myclean_add_library(rules
  SOURCES
    catalog.cpp
    catalog.hpp
    data_files.cpp
    data_files.hpp
    parse.hpp
    parse_kb.cpp
    parse_support.cpp
    parse_support.hpp
    parse_toml.cpp
    schema.cpp
    schema.hpp
  DEPENDS
    myclean::model
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus)
