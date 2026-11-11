# What the service host needs from Windows: the SCM dispatcher, the state folder and its ACL, the
# console user, and crash dumps.
myclean_add_library(platform_service
  SOURCES
    crash_dump.cpp
    crash_dump.hpp
    scm.cpp
    scm.hpp
    system.cpp
    system.hpp
  DEPENDS
    myclean::platform_fs
    myclean::platform_ipc
    WIL::WIL
    advapi32
    dbghelp
    ole32
    shell32
    wtsapi32)
