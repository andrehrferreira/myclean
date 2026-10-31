# Transparent compression (compression spec): the sampled analyzer over a scan, the eligibility
# rules, and the CompressFolder (WOF) and CompactOs actions over the probe, WOF and command-runner
# interfaces that src/platform/compress and src/platform/cleanup implement.
myclean_add_library(compress
  SOURCES
    analyzer.cpp
    analyzer.hpp
    compact_os.cpp
    compact_os.hpp
    compress_folder.cpp
    compress_folder.hpp
    compression.cpp
    compression.hpp
  DEPENDS
    myclean::cleanup
    myclean::model
    myclean::os
    myclean::packs
    myclean::space)
