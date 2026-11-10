# Search-indexer scope (optimizer spec, "Search-Indexer Scope"): the interface to the Windows
# Search crawl scope, the build-output and cache folders it indexes (attribution areas of
# categories build-artifacts, dev-caches and app-caches, and the targets of the project clean
# cases below the workspace roots), and the IndexerExclusion action that keeps them out with
# exact undo. The Win32 crawl-scope manager lives in src/platform/indexer.
myclean_add_library(indexer
  SOURCES
    exclusion.cpp
    exclusion.hpp
    folders.cpp
    folders.hpp
    scope.cpp
    scope.hpp
  DEPENDS
    myclean::clean
    myclean::map
    myclean::model
    myclean::os
    myclean::rules
    nlohmann_json::nlohmann_json)
