# Resource governor core (governor spec, .rulebook/tasks/phase3_resource-governor): process
# classification, the policy of data/governor.toml, pressure detection, process trees and the
# escalation ladder, game mode, job placement, owner notifications, orphaned polling loops and
# crash reconciliation.
# Pure decisions only: snapshots in, commands out. The Win32 side lives in src/platform.
myclean_add_library(governor
  SOURCES
    classifier.cpp
    classifier.hpp
    commands.cpp
    commands.hpp
    forest.cpp
    forest.hpp
    game_mode.cpp
    game_mode.hpp
    jobs.cpp
    jobs.hpp
    ladder.cpp
    ladder.hpp
    notify.cpp
    notify.hpp
    orphans.cpp
    orphans.hpp
    policy.cpp
    policy.hpp
    pressure.cpp
    pressure.hpp
    reason.cpp
    reason.hpp
    recovery.cpp
    recovery.hpp
  DEPENDS
    myclean::model
    myclean::rules
    myclean::telemetry
    tomlplusplus::tomlplusplus)
