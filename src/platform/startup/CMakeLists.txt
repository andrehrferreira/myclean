# Win32 reader and writer of the startup sources (core/startup): Run and RunOnce keys in both
# registry views, Startup folders, StartupApproved values, scheduled tasks (ITaskService) and
# services, with the signer of each task and service image.
myclean_add_library(platform_startup
  SOURCES
    win32_startup.cpp
    win32_startup.hpp
  DEPENDS
    myclean::startup
    myclean::platform_settings
    myclean::platform_trust
    WIL::WIL
    advapi32
    ole32
    oleaut32
    shell32
    taskschd
    uuid)
