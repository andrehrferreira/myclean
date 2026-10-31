# The service body in-process: console mode, a temporary state folder and a unique pipe name per
# test (never \.\pipe\myclean). Nothing here installs or starts a Windows service.
myclean_add_test(svc
  SOURCES
    fake_machine.hpp
    host_test.cpp
    service_test.cpp
    svc_test_support.hpp
    telemetry_component_test.cpp
  DEPENDS
    myclean::svc
    myclean::fakes)
target_compile_definitions(myclean_svc_tests PRIVATE MYCLEAN_TEST_VERSION="${PROJECT_VERSION}")
# The fakes of the telemetry sources: "telemetry/fake_sources.hpp".
target_include_directories(myclean_svc_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit")
