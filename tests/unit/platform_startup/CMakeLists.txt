# The startup reader against this machine, read paths only, plus the pure parsing pieces. The
# only write is a throwaway HKCU StartupApproved\Run value with no Run value, deleted by the
# test; no task, service, Run value or HKLM key is ever changed.
myclean_add_test(platform_startup
  SOURCES
    startup_detail_test.cpp
    win32_startup_test.cpp
  DEPENDS
    myclean::platform_startup
    myclean::platform_ipc)
