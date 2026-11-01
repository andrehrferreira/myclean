# The Win32 game sources against the real machine: reads only, and a folder inside
# %TEMP%\myclean-it.
myclean_add_test(platform_games
  SOURCES
    win32_games_test.cpp
  DEPENDS
    myclean::platform_games)
