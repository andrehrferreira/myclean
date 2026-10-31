# Placement: relocation of TEMP/TMP, the package caches and what the knowledge packs' relocation
# recipes name, through their supported settings and commands, and the approval-only system space
# settings (hibernation, page file, shadow storage), each an Action with exact undo. The Win32
# adapters live in src/platform/settings.
myclean_add_library(placement
  SOURCES
    drives.cpp
    drives.hpp
    npmrc.cpp
    npmrc.hpp
    proposal.cpp
    proposal.hpp
    recipe.cpp
    recipe.hpp
    relocation.cpp
    relocation.hpp
    settings.cpp
    settings.hpp
    system_space.cpp
    system_space.hpp
  DEPENDS
    myclean::actions
    myclean::cleanup
    myclean::health
    myclean::model
    myclean::os
    myclean::packs
    myclean::safety
    myclean::telemetry
    nlohmann_json::nlohmann_json)
