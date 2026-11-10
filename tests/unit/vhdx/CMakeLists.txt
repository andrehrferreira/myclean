myclean_add_test(vhdx
  SOURCES
    compact_test.cpp
    report_test.cpp
    vhdx_fakes.hpp
  DEPENDS
    myclean::vhdx
    myclean::fakes)
# The fakes of other modules' tests: "clean/clean_fakes.hpp".
target_include_directories(myclean_vhdx_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
