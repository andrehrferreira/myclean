myclean_add_test(startup
  SOURCES
    startup_fakes.hpp
    startup_test.cpp
    toggle_test.cpp
  DEPENDS
    myclean::startup
    myclean::fakes)
