# The handler host over a fake handler and against the real machine (read only: Initialize and
# GetSpaceUsed), the command runner over harmless sandbox scripts and the $I records over a
# temporary folder; the shell's Recycle Bin (read, plus one file of the test's own recycled and
# deleted, with no UI) only with MYCLEAN_TEST_RECYCLE_BIN=1. Pass with or without elevation.
myclean_add_test(platform_cleanup
  SOURCES
    command_runner_test.cpp
    handler_host_test.cpp
    handler_probe_test.cpp
    recycle_bin_test.cpp
  DEPENDS
    myclean::platform_cleanup)
