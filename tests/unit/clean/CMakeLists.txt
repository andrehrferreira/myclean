myclean_add_test(clean
  SOURCES
    artifacts_test.cpp
    catalog_test.cpp
    clean_fakes.hpp
    engine_test.cpp
    fixture.hpp
    patterns_test.cpp
    workspaces_test.cpp
  DEPENDS
    myclean::clean)

# The shipped data folder: the scenario tests evaluate the rules of data/rules.
target_compile_definitions(myclean_clean_tests PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
