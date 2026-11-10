# core/devdrive IDevDriveProbe: the Windows build and each fixed volume's file system and Dev
# Drive state. Read-only.
myclean_add_library(platform_devdrive
  SOURCES
    win32_dev_drive.cpp
    win32_dev_drive.hpp
  DEPENDS
    myclean::devdrive
    WIL::WIL
    advapi32)
