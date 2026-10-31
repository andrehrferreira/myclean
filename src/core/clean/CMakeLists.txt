# The cleaning rule engine (C1) and the developer-artifact scanner (C2): what the rules select.
myclean_add_library(clean
  SOURCES
    engine.cpp
    engine.hpp
    patterns.cpp
    patterns.hpp
    probes.hpp
    walk.cpp
    walk.hpp
    workspaces.cpp
    workspaces.hpp
  DEPENDS
    myclean::rules
    myclean::safety
    tomlplusplus::tomlplusplus)
