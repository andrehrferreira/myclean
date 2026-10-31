# The compression analyzer, the eligibility rules and the CompressFolder and CompactOs actions,
# over a fake probe, a fake WOF, a fake directory reader and a fake command runner.
myclean_add_test(compress
  SOURCES
    actions_test.cpp
    analyzer_test.cpp
    compression_test.cpp
    fakes.hpp
  DEPENDS
    myclean::compress
    myclean::fakes)
