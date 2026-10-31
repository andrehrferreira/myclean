# Smoke tests of the process table against the real machine: plausible ranges, not exact values.
myclean_add_test(platform_proc
  SOURCES
    nt_process_table_test.cpp
  DEPENDS
    myclean::platform_proc)
