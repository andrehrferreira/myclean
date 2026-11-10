myclean_add_test(devdrive
  SOURCES
    advisor_test.cpp
  DEPENDS
    myclean::devdrive)
# The fakes of other modules' tests: "clean/clean_fakes.hpp".
target_include_directories(myclean_devdrive_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
