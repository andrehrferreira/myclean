# The Win32 settings adapters against the real machine: read paths only, and file operations
# inside %TEMP%\myclean-it. Nothing here changes a variable, tool configuration or system setting.
myclean_add_test(platform_settings
  SOURCES
    machine_report_test.cpp
    sandbox.hpp
    settings_test.cpp
  DEPENDS
    myclean::platform_settings
    myclean::platform_disk
    myclean::platform_disk_health
    myclean::platform_fs
    myclean::fakes)
