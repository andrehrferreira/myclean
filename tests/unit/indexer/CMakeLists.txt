myclean_add_test(indexer
  SOURCES
    exclusion_test.cpp
    fake_scope.hpp
    folders_test.cpp
  DEPENDS
    myclean::indexer
    myclean::fakes)
# The cleaning engine's in-memory file tree and environment: "clean/clean_fakes.hpp".
target_include_directories(myclean_indexer_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
