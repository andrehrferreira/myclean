# The cleanup actions over a fake runner, a fake handler host and FakeFileSystem; DISM output
# from the fixtures folder (see dism_cleanup_test.cpp for where each one comes from).
myclean_add_test(cleanup
  SOURCES
    dism_cleanup_test.cpp
    fakes.hpp
    handler_cleanup_test.cpp
    native_command_test.cpp
  DEPENDS
    myclean::cleanup
    myclean::fakes)
target_compile_definitions(myclean_cleanup_tests
  PRIVATE MYCLEAN_CLEANUP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
