# The handler host over a fake handler and against the real machine (read only: Initialize and
# GetSpaceUsed), and the command runner over harmless sandbox scripts. Pass with or without
# elevation.
myclean_add_test(platform_cleanup
  SOURCES
    command_runner_test.cpp
    handler_host_test.cpp
    handler_probe_test.cpp
  DEPENDS
    myclean::platform_cleanup)
