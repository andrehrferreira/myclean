# core/space IImageDecoder through the Windows Imaging Component (near-duplicate photos).
myclean_add_library(platform_image
  SOURCES
    wic_image_decoder.cpp
    wic_image_decoder.hpp
  DEPENDS
    myclean::space
    myclean::platform_scan
    WIL::WIL
    ole32
    windowscodecs)
