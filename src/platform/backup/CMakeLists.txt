# core/backup's IBackupSystem on Windows (spike X8): File History configuration, wbadmin, restore
# points and shadow copies over WMI, OneDrive folder backup, installed backup products and the
# volume labels. Read-only.
myclean_add_library(platform_backup
  SOURCES
    win32_backup.cpp
    win32_backup.hpp
  DEPENDS
    myclean::backup
    myclean::platform_git
    WIL::WIL
    advapi32
    ole32
    oleaut32
    wbemuuid)
