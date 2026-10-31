# Knowledge packs (docs/packs.md): discovery and validation of data/packs and the owner's override
# packs, applies_when, the namespaced registry that feeds the engines, and the fixture runner.
# Read-only.
myclean_add_library(packs
  SOURCES
    applies.cpp
    applies.hpp
    fields.cpp
    fields.hpp
    fixtures.cpp
    fixtures.hpp
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
target_compile_definitions(myclean_packs PRIVATE MYCLEAN_VERSION="${PROJECT_VERSION}")
