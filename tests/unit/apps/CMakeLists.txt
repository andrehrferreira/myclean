myclean_add_test(apps
  SOURCES
    apps_test.cpp
  DEPENDS
    myclean::apps
    myclean::fakes)
# The placement test fakes ("placement/placement_fakes.hpp").
target_include_directories(myclean_apps_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
