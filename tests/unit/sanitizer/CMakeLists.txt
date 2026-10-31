# Only meaningful when AddressSanitizer is on (the debug preset).
if(MYCLEAN_ASAN)
  myclean_add_test(sanitizer SOURCES asan_test.cpp)
endif()
