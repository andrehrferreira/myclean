# The backup reader against this machine, read paths only (volumes, Uninstall keys, the owner's
# File History and OneDrive state), plus the WMI datetime parsing. No tool is started and no
# backup setting is changed.
myclean_add_test(platform_backup
  SOURCES
    win32_backup_test.cpp
  DEPENDS
    myclean::platform_backup
    myclean::platform_ipc)
