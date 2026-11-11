# The intruder scan's readers against this machine, read paths only: persistence sources,
# target resolution, the user's browser profiles and Defender's detection history. No file is
# scanned, no lookup leaves the machine and nothing is written.
myclean_add_test(platform_security
  SOURCES
    win32_security_test.cpp
  DEPENDS
    myclean::platform_security)
