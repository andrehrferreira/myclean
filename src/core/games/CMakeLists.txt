# Game detection for the resource governor (spec Game Detection, spike X5): Steam, Epic, GOG and
# Xbox library roots, the owner's GameConfigStore entries and list, and the fullscreen + GPU
# signal. Win32 readers of these sources live in src/platform/games.
myclean_add_library(games
  SOURCES
    detector.cpp
    detector.hpp
    epic.cpp
    epic.hpp
    fullscreen.cpp
    fullscreen.hpp
    game_config_store.cpp
    game_config_store.hpp
    library_roots.cpp
    library_roots.hpp
    owner_games.cpp
    owner_games.hpp
    paths.cpp
    paths.hpp
    steam.cpp
    steam.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::telemetry
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus)
