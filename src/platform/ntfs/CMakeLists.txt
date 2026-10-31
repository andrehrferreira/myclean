# Win32 volume access for the NTFS index: raw $MFT reads and the read-only NTFS and USN journal
# FSCTLs over \\.\X: (elevated).
myclean_add_library(platform_ntfs
  SOURCES
    win32_ntfs_volume.cpp
    win32_ntfs_volume.hpp
  DEPENDS
    myclean::index
    WIL::WIL
    advapi32)
