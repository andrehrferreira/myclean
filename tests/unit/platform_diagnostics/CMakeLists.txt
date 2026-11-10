# The Diagnostics-Performance log reader against this machine, read-only. A non-elevated run may
# not read the log; the test then checks that the refusal is reported as access_denied.
myclean_add_test(platform_diagnostics
  SOURCES
    win32_diagnostics_log_test.cpp
  DEPENDS
    myclean::platform_diagnostics)
