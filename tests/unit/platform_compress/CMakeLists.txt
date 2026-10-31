# The probe and WOF adapters against the real file system, only inside a unique folder under
# %TEMP%\myclean-it (compress, decompress, CompressFolder end to end), plus the read-only
# CompactOS state of this machine. Pass with or without elevation.
myclean_add_test(platform_compress
  SOURCES
    sandbox.cpp
    sandbox.hpp
    win32_compression_test.cpp
  DEPENDS
    myclean::platform_compress
    myclean::platform_scan
    WIL::WIL)
