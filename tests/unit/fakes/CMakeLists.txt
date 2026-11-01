myclean_add_test(fakes
  SOURCES
    fake_clock_test.cpp
    fake_file_system_test.cpp
    fake_jobs_test.cpp
  DEPENDS
    myclean::fakes)
