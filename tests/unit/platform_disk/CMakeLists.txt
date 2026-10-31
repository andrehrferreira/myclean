# Smoke tests of the volume source against the real machine: plausible ranges, not exact values.
myclean_add_test(platform_disk
  SOURCES
    win32_volume_info_test.cpp
  DEPENDS
    myclean::platform_disk)
