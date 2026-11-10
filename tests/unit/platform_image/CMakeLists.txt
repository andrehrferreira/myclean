# Adapter tests of the WIC decoder on images the test encodes with WIC inside a unique folder
# under %TEMP%\myclean-it.
myclean_add_test(platform_image
  SOURCES
    wic_image_decoder_test.cpp
  DEPENDS
    myclean::platform_image
    ole32
    windowscodecs)
