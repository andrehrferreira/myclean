# Win32 readers of the intruder scan (core/security): WMI event subscriptions, IFEO, Winlogon and
# AppInit values, the owner's browser profiles and extension policies, Defender's detection
# history and its MpCmdRun file scan (with -DisableRemediation), the opt-in VirusTotal hash lookup
# (WinHTTP), reverse DNS (getnameinfo) and the owner's security.toml.
myclean_add_library(platform_security
  SOURCES
    win32_browsers.cpp
    win32_defender.cpp
    win32_network.cpp
    win32_persistence.cpp
    win32_security.hpp
    wmi.cpp
    wmi.hpp
  DEPENDS
    myclean::security
    myclean::platform_packs
    myclean::platform_service
    myclean::platform_settings
    WIL::WIL
    advapi32
    ole32
    oleaut32
    shell32
    wbemuuid
    winhttp
    ws2_32)
