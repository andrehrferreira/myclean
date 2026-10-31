# Adapter tests of platform_clean and platform_git against the real file system and git, inside
# a unique folder under %TEMP%\myclean-it (the platform_fs sandbox). smoke_test.cpp is disabled:
# it reads the owner's real folders and reports what the shipped catalog would select.
myclean_add_test(platform_clean
  SOURCES
    ../platform_fs/sandbox.cpp
    ../platform_fs/sandbox.hpp
    git_test.cpp
    probes_test.cpp
    shipped_rules.hpp
    smoke_test.cpp
  DEPENDS
    myclean::platform_clean
    myclean::platform_fs
    myclean::platform_git
    myclean::packs)

target_include_directories(myclean_platform_clean_tests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../platform_fs")
target_compile_definitions(myclean_platform_clean_tests
  PRIVATE MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
