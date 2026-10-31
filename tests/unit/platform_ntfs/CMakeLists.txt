# Volume adapter checks: refusal without elevation, the USN parser against what Windows returns
# for a real file, and (elevated only) the root record and journal of C:. The X7 measurements
# run only when MYCLEAN_X7_FOLDER / MYCLEAN_X7_VOLUME are set (docs/spikes/X7-ntfs-index.md).
myclean_add_test(platform_ntfs
  SOURCES
    win32_ntfs_volume_test.cpp
  DEPENDS
    myclean::platform_ntfs
    myclean::platform_scan)
