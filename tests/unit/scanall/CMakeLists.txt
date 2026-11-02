myclean_add_test(scanall
  SOURCES
    analyzers_test.cpp
    archives_test.cpp
    drive_health_test.cpp
    duplicates_test.cpp
    report_test.cpp
    scanall_fakes.hpp
    sources_test.cpp
    worktrees_test.cpp
  DEPENDS
    myclean::scanall)

# The space test fakes ("space/space_fakes.hpp") and this folder's ("scanall/scanall_fakes.hpp").
target_include_directories(myclean_scanall_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
