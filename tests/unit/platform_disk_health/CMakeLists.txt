# Smoke tests of the disk-health reader against the real machine; pass with or without elevation.
myclean_add_test(platform_disk_health
  SOURCES
    win32_disk_health_test.cpp
  DEPENDS
    myclean::platform_disk_health)
