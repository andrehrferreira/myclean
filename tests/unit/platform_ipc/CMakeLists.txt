# The pipe server and client in-process, on a unique pipe name per test (never \\.\pipe\myclean).
myclean_add_test(platform_ipc
  SOURCES
    pipe_test.cpp
    security_test.cpp
  DEPENDS
    myclean::platform_ipc)
