# Whole-volume NTFS index (S3): $MFT and USN record parsing, the in-memory folder tree with
# totals, its USN journal updates, the scanner v1 shape and the per-folder size history.
# Win32 volume access in platform/ntfs.
myclean_add_library(index
  SOURCES
    bytes.hpp
    folder_history.cpp
    folder_history.hpp
    indexed_volume.cpp
    indexed_volume.hpp
    mft.cpp
    mft.hpp
    usn.cpp
    usn.hpp
    volume.cpp
    volume.hpp
    volume_index.cpp
    volume_index.hpp
  DEPENDS
    myclean::space
    myclean::store)
