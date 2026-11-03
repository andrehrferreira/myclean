myclean_add_test(scanall
  SOURCES
    analyzers_test.cpp
    archives_test.cpp
    drive_health_test.cpp
    duplicates_test.cpp
    owner_decisions_test.cpp
    report_test.cpp
    scanall_fakes.hpp
    sources_test.cpp
    worktrees_test.cpp
  DEPENDS
    myclean::fakes
    myclean::scanall)

# The space, clean and placement test fakes ("space/space_fakes.hpp", "clean/clean_fakes.hpp",
# "placement/placement_fakes.hpp") and this folder's ("scanall/scanall_fakes.hpp").
target_include_directories(myclean_scanall_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
