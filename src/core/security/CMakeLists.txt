# The intruder scan (security spec): persistence points (the startup items plus WMI event
# subscriptions, IFEO debuggers, Winlogon Shell/Userinit and AppInit_DLLs), browser extensions
# with their install origin, background network talkers over time with reverse DNS, and
# verdicts only from Microsoft Defender or the opt-in, hash-only VirusTotal lookup; tables
# visible_window and file_verdict. Reads only. The Win32 readers live in src/platform/security.
myclean_add_library(security
  SOURCES
    extensions.cpp
    extensions.hpp
    network.cpp
    network.hpp
    persistence.cpp
    persistence.hpp
    scan.cpp
    scan.hpp
    verdicts.cpp
    verdicts.hpp
  DEPENDS
    myclean::events
    myclean::ipc
    myclean::model
    myclean::startup
    myclean::store
    myclean::trust
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus)
