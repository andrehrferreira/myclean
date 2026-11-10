# Scanner v1 (folder tree with sizes on disk) and the read-only space analyses: top-N, old
# downloads, used installers, exact duplicates, near-duplicate photos, and their JSON forms.
# Win32 in platform/scan and platform/image.
myclean_add_library(space
  SOURCES
    content.hpp
    downloads.cpp
    downloads.hpp
    duplicates.cpp
    duplicates.hpp
    identity_set.hpp
    installers.cpp
    installers.hpp
    json.cpp
    json.hpp
    photos.cpp
    photos.hpp
    scanner.cpp
    scanner.hpp
    text.hpp
    top.cpp
    top.hpp
  DEPENDS
    myclean::os
    nlohmann_json::nlohmann_json)
