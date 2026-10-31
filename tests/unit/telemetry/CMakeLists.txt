# Telemetry collectors, pipeline and history against in-memory fakes of the OS sources.
myclean_add_test(telemetry
  SOURCES
    fake_sources.hpp
    gpu_collector_test.cpp
    history_test.cpp
    pipeline_test.cpp
    process_collector_test.cpp
    system_collectors_test.cpp
    temp_store.hpp
  DEPENDS
    myclean::telemetry
    myclean::fakes)
