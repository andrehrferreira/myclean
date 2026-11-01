# Win32 sources of game detection (core/games): Steam, Epic, GOG and XboxGames installs, the
# owner's GameConfigStore and SHQueryUserNotificationState. Read-only.
myclean_add_library(platform_games
  SOURCES
    win32_games.cpp
    win32_games.hpp
  DEPENDS
    myclean::games
    WIL::WIL
    advapi32
    ole32
    shell32)
