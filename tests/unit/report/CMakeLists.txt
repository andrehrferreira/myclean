myclean_add_test(report
  SOURCES
    report_fakes.hpp
    report_test.cpp
  DEPENDS
    myclean::actions
    myclean::fakes
    myclean::governing
    myclean::input
    myclean::report)
# The fakes of the service's read methods ("report_fakes.hpp") and the clock ("fakes/...").
target_include_directories(myclean_report_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
