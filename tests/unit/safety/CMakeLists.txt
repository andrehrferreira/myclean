myclean_add_test(safety
  SOURCES
    paths_test.cpp
    safe_remove_test.cpp
  DEPENDS
    myclean::safety
    myclean::fakes)
