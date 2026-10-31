# The service body in-process: console mode, a temporary state folder and a unique pipe name per
# test (never \\.\pipe\myclean). Nothing here installs or starts a Windows service.
myclean_add_test(svc
  SOURCES
    host_test.cpp
    service_test.cpp
    svc_test_support.hpp
  DEPENDS
    myclean::svc
    myclean::fakes)
target_compile_definitions(myclean_svc_tests PRIVATE MYCLEAN_TEST_VERSION="${PROJECT_VERSION}")
