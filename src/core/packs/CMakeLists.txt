# Knowledge packs (docs/packs.md): discovery and validation of data/packs and the owner's override
# packs, applies_when, and the namespaced registry that feeds the engines. Read-only.
myclean_add_library(packs
  SOURCES
    applies.cpp
    applies.hpp
    fields.cpp
    fields.hpp
    loader.cpp
    loader.hpp
    parse.cpp
    parse.hpp
    registry.cpp
    registry.hpp
    schema.hpp
  DEPENDS
    myclean::clean
    myclean::map
    myclean::rules
    tomlplusplus::tomlplusplus)
