myclean_add_test(map
  SOURCES
    attribution_test.cpp
    disk_map_test.cpp
  DEPENDS
    myclean::map
    myclean::placement)

# The space test fakes ("space/space_fakes.hpp") and the shipped data folder: the attribution
# tests load data/attribution.toml and check its references against data/rules.
target_include_directories(myclean_map_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
target_compile_definitions(myclean_map_tests PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
