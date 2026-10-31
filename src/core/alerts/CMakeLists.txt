# Alert engine: threshold and change rules from data/alerts.toml, stored alert records, rate
# limiting, snooze and the active set behind the tray badge.
myclean_add_library(alerts
  SOURCES
    config.cpp
    config.hpp
    engine.cpp
    engine.hpp
    schema.cpp
    schema.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::rules
    myclean::store
    tomlplusplus::tomlplusplus)
