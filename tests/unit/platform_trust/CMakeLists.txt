# Signature checks against real files of this machine and an unsigned test executable.
myclean_add_test(platform_trust
  SOURCES
    win_image_inspector_test.cpp
  DEPENDS
    myclean::platform_trust)
