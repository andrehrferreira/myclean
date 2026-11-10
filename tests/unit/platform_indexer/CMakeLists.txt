# The crawl-scope reader against this machine, read paths only: no index rule is ever added or
# removed here.
myclean_add_test(platform_indexer
  SOURCES
    win32_indexer_scope_test.cpp
  DEPENDS
    myclean::platform_indexer)
