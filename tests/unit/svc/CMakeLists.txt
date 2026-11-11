# The service body in-process: console mode, a temporary state folder and a unique pipe name per
# test (never \.\pipe\myclean). Nothing here installs or starts a Windows service.
myclean_add_test(svc
  SOURCES
    alerts_component_test.cpp
    backup_component_test.cpp
    cleaning_component_test.cpp
    cleaning_smoke_test.cpp
    disk_health_component_test.cpp
    events_adapter_test.cpp
    fake_machine.hpp
    governor_component_test.cpp
    host_test.cpp
    input_component_test.cpp
    phase1_fakes.hpp
    phase1_service_test.cpp
    resident_component_test.cpp
    service_test.cpp
    svc_test_support.hpp
    telemetry_component_test.cpp
    trust_component_test.cpp
    ../platform_fs/sandbox.cpp
    ../platform_fs/sandbox.hpp
  DEPENDS
    myclean::svc
    myclean::fakes)
target_compile_definitions(myclean_svc_tests PRIVATE MYCLEAN_TEST_VERSION="${PROJECT_VERSION}"
  MYCLEAN_DATA_DIR="${PROJECT_SOURCE_DIR}/data" MYCLEAN_SVC_EXE="$<TARGET_FILE:myclean-svc>")
# The console smoke test starts the real executable (with a cleaning sandbox).
add_dependencies(myclean_svc_tests myclean-svc)
# The fakes of the telemetry and event sources: "telemetry/fake_sources.hpp",
# "events/event_fakes.hpp".
target_include_directories(myclean_svc_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
