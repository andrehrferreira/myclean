# The Win32 side of the knowledge packs against the real machine: reads only, and folders inside
# %TEMP%\myclean-it.
myclean_add_test(platform_packs
  SOURCES
    win32_packs_test.cpp
  DEPENDS
    myclean::platform_packs)