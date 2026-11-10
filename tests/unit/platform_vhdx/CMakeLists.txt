# Compaction of small virtual disks the test creates inside a unique folder under
# %TEMP%\myclean-it (never attached), and where wsl.exe is looked for. WSL is never run.
myclean_add_test(platform_vhdx
  SOURCES
    win32_vhdx_test.cpp
  DEPENDS
    myclean::platform_vhdx
    virtdisk)
