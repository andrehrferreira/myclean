# myclean.exe end to end: the CLI and the MCP server in-process against a console-mode service
# in the same process (telemetry over fakes, a unique pipe per test), and the built executable
# for deploy contract C6. Nothing here installs or starts a Windows service.
myclean_add_test(cli
  SOURCES
    cli_test.cpp
    text_guard_test.cpp
  DEPENDS
    myclean::cli
    myclean::svc
    myclean::fakes)
# The service test helpers ("svc/fake_machine.hpp") and the telemetry fakes they use.
target_include_directories(myclean_cli_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
target_compile_definitions(myclean_cli_tests PRIVATE MYCLEAN_CLI_EXE="$<TARGET_FILE:myclean>"
  MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
add_dependencies(myclean_cli_tests myclean)
