myclean_add_test(packs
  SOURCES
    applies_test.cpp
    fixtures_test.cpp
    loader_test.cpp
    pack_fakes.hpp
    parse_test.cpp
    shipped_packs_test.cpp
  DEPENDS
    myclean::packs)

# The applicability tests use the cleaning engine's in-memory probes (FakeTree, FakeEnvironment).
target_include_directories(myclean_packs_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/clean")
# The shipped packs (data/packs) and the version they are loaded as.
target_compile_definitions(myclean_packs_tests PRIVATE
  MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
  MYCLEAN_VERSION="${PROJECT_VERSION}")
