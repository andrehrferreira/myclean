# Game detection from fixture text: launcher files, GameConfigStore entries and the fullscreen +
# GPU signal over a FakeClock.
myclean_add_test(games
  SOURCES
    detector_test.cpp
    epic_test.cpp
    fullscreen_test.cpp
    library_roots_test.cpp
    steam_test.cpp
  DEPENDS
    myclean::games
    myclean::fakes)
