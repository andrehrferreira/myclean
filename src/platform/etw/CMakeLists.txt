# Win32 event sources (core/events): the ETW session on the kernel process and network providers
# (krabsetw), process details for its events, and the TCP/UDP tables (IP Helper).
myclean_add_library(platform_etw
  SOURCES
    etw_event_source.cpp
    etw_event_source.hpp
    etw_records.cpp
    etw_records.hpp
    ip_helper_connections.cpp
    ip_helper_connections.hpp
    krabs_user.hpp
    process_details.cpp
    process_details.hpp
  DEPENDS
    myclean::events
    krabsetw::krabsetw
    WIL::WIL
    iphlpapi)
