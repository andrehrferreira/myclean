# Smoke tests of the PDH and memory sources and of the whole telemetry pipeline against the real
# machine: plausible ranges, not exact values. The pipeline test prints the collectors' overhead.
myclean_add_test(platform_perf
  SOURCES
    pdh_counters_test.cpp
    pipeline_smoke_test.cpp
  DEPENDS
    myclean::platform_disk
    myclean::platform_perf
    myclean::platform_proc
    myclean::store)
