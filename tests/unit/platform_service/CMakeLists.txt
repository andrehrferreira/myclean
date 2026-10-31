# Service host adapters. Nothing here registers, starts or changes a Windows service.
myclean_add_test(platform_service
  SOURCES
    crash_dump_test.cpp
    scm_test.cpp
    system_test.cpp
    temp_dir.hpp
  DEPENDS
    myclean::platform_service)
