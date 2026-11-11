myclean_add_test(backup
  SOURCES
    backup_test.cpp
  DEPENDS
    myclean::backup)
# The space test fakes ("space/space_fakes.hpp").
target_include_directories(myclean_backup_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
