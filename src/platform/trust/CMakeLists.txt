# Win32 facts for the trust signal: Authenticode (WinVerifyTrust, catalogs) and user-writable
# folders (core/trust/trust.hpp).
myclean_add_library(platform_trust
  SOURCES
    win_image_inspector.cpp
    win_image_inspector.hpp
  DEPENDS
    myclean::trust
    WIL::WIL
    wintrust
    crypt32
    shell32
    ole32
    uuid)
