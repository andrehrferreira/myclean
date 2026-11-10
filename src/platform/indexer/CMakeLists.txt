# The Windows Search crawl scope (core/indexer::IIndexerScope) through ISearchManager and
# ISearchCrawlScopeManager of catalog SystemIndex.
myclean_add_library(platform_indexer
  SOURCES
    win32_indexer_scope.cpp
    win32_indexer_scope.hpp
  DEPENDS
    myclean::indexer
    WIL::WIL
    ole32)
