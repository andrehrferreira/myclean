# Adapter tests against the real file system, inside a unique folder under %TEMP%\myclean-it.
myclean_add_test(platform_fs
  SOURCES
    protected_locations_test.cpp
    safe_remove_on_disk_test.cpp
    sandbox.cpp
    sandbox.hpp
    win32_file_system_test.cpp
  DEPENDS
    myclean::platform_fs
    myclean::safety)
