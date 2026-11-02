# The cleaning service over fakes: the file system, the rule probes, handlers, system settings,
# drives, the space reader and the Recycle Bin (see cleaning_fixture.hpp), and the round trip of a
# user task through user_tasks. Nothing here touches a real file.
myclean_add_test(cleaning
  SOURCES
    cleaning_fixture.hpp
    executor_test.cpp
    recycle_bin_test.cpp
    service_test.cpp
  DEPENDS
    myclean::cleaning
    myclean::fakes
    myclean::packs
    myclean::user_tasks)
# The fakes of other modules' tests: "clean/clean_fakes.hpp", "cleanup/fakes.hpp", ...
target_include_directories(myclean_cleaning_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
