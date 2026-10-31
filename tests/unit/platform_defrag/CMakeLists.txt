# The drive-maintenance readers against the real machine, read-only and within small budgets.
myclean_add_test(platform_defrag
  SOURCES
    defrag_test.cpp
  DEPENDS
    myclean::platform_defrag
    myclean::platform_disk
    myclean::platform_disk_health
    myclean::platform_scan
    myclean::platform_settings)
