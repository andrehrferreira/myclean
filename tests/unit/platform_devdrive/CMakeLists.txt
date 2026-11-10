# Read-only checks of the Windows build and of the system volume's file system.
myclean_add_test(platform_devdrive
  SOURCES
    win32_dev_drive_test.cpp
  DEPENDS
    myclean::platform_devdrive)
