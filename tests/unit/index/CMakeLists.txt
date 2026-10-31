# NTFS index: $MFT and USN record parsing against fixtures built from the documented structures
# (and one USN record recorded on the reference machine), the index built from an in-memory
# volume and compared with scanner v1, journal updates and rebuilds, and the folder history.
myclean_add_test(index
  SOURCES
    folder_history_test.cpp
    indexed_volume_test.cpp
    mft_test.cpp
    ntfs_image.cpp
    ntfs_image.hpp
    tree_model.hpp
    usn_test.cpp
    volume_index_test.cpp
  DEPENDS
    myclean::index
    myclean::fakes)
