# Win32 side of transparent compression (core/compress): the sampling probe (RtlCompressBuffer)
# and WOF compression, decompression and state (wofutil), all at background priority.
myclean_add_library(platform_compress
  SOURCES
    win32_compression.cpp
    win32_compression.hpp
  DEPENDS
    myclean::compress
    myclean::platform_scan
    WIL::WIL
    wofutil)
