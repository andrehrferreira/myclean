# Disk-health interpretation, analysis, history and collector, against byte blocks recorded on
# the reference machine (fixtures/) and the shipped data/drive-specs.json.
myclean_add_test(health
  SOURCES
    analysis_test.cpp
    collector_test.cpp
    drive_specs_test.cpp
    health_test_support.hpp
    reading_test.cpp
  DEPENDS
    myclean::health
    myclean::fakes)

target_compile_definitions(myclean_health_tests
  PRIVATE
    MYCLEAN_HEALTH_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    MYCLEAN_HEALTH_DATA="${PROJECT_SOURCE_DIR}/data")
