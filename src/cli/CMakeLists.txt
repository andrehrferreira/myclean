# myclean.exe: the read-only CLI (`--json` for scripts, `status --json` for deploy contract C6)
# and `myclean mcp`, the stdio MCP server. The library behind it is driven in-process by tests.
myclean_add_library(cli
  SOURCES
    cli.cpp
    cli.hpp
    map_command.cpp
    map_command.hpp
    packs_command.cpp
    packs_command.hpp
    scan_all_command.cpp
    scan_all_command.hpp
    text.cpp
    text.hpp
  DEPENDS
    myclean::map
    myclean::mcp
    myclean::os
    myclean::packs
    myclean::platform_clean
    myclean::platform_compress
    myclean::platform_defrag
    myclean::platform_disk
    myclean::platform_disk_health
    myclean::platform_fs
    myclean::platform_git
    myclean::platform_ipc
    myclean::platform_ntfs
    myclean::platform_packs
    myclean::platform_scan
    myclean::platform_settings
    myclean::scanall)
target_compile_definitions(myclean_cli PRIVATE MYCLEAN_VERSION="${PROJECT_VERSION}")

add_executable(myclean main.cpp ${PROJECT_SOURCE_DIR}/src/resources/myclean.rc)
target_link_libraries(myclean PRIVATE myclean::cli)
if(MYCLEAN_ASAN)
  add_custom_command(TARGET myclean POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MYCLEAN_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:myclean>"
    VERBATIM)
endif()
myclean_install_executable(myclean)
_myclean_register_lint_sources(main.cpp)
