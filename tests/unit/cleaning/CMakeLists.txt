# The cleaning service over fakes: the file system, the rule probes, handlers, system settings,
# drives and the space reader (see cleaning_fixture.hpp). Nothing here touches a real file.
myclean_add_test(cleaning
  SOURCES
    cleaning_fixture.hpp
    service_test.cpp
  DEPENDS
    myclean::cleaning
    myclean::fakes)
# The fakes of other modules' tests: "clean/clean_fakes.hpp", "cleanup/fakes.hpp", ...
target_include_directories(myclean_cleaning_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
