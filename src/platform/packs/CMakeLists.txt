# Win32 side of the knowledge packs: packs::IMachine (installed programs and Appx packages,
# running images, the Windows build) and the ACL check of the owner's override folder.
myclean_add_library(platform_packs
  SOURCES
    win32_packs.cpp
    win32_packs.hpp
  DEPENDS
    myclean::packs
    WIL::WIL
    advapi32
    ole32
    shell32)
