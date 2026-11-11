# Installed apps (apps spec): the Uninstall keys and Appx packages with size, publisher, the
# knowledge-base description, last use fused from UserAssist, Prefetch, BAM and MyClean's own
# process history (with source and confidence), the keep/remove verdict, and the parsers of those
# usage sources. Read-only; the Win32 reads live in src/platform/apps.
myclean_add_library(apps
  SOURCES
    apps.cpp
    apps.hpp
  DEPENDS
    myclean::model
    myclean::placement
    myclean::rules
    myclean::scanall
    myclean::store
    myclean::trust
    nlohmann_json::nlohmann_json)
