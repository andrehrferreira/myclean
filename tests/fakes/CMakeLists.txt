# In-memory fakes of the core/os interfaces, for any module's tests:
#   target_link: myclean::fakes    include: "fakes/fake_file_system.hpp"
myclean_add_library(fakes
  SOURCES
    fake_clock.cpp
    fake_clock.hpp
    fake_file_system.cpp
    fake_file_system.hpp
  DEPENDS
    myclean::os
  INCLUDE_ROOT
    "${PROJECT_SOURCE_DIR}/tests")
