# Backup detection (backup spec): the Windows backup sources, third-party backup products, the
# artifacts on the backup volumes, the last-backup age and its alert reading, the recommendation
# when no backup exists, and the read-only repository safety check. The Win32 reads live in
# src/platform/backup, git in src/platform/git.
myclean_add_library(backup
  SOURCES
    backup.cpp
    backup.hpp
    repos.cpp
    repos.hpp
  DEPENDS
    myclean::alerts
    myclean::model
    myclean::placement
    myclean::space
    myclean::store
    nlohmann_json::nlohmann_json)
