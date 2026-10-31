# ETW record conversion from recorded fixtures, process details and the TCP/UDP tables against
# the real machine, and smoke tests of the real ETW session that skip without the rights to
# enable the kernel providers.
myclean_add_test(platform_etw
  SOURCES
    etw_event_source_test.cpp
    etw_records_test.cpp
    ip_helper_connections_test.cpp
    process_details_test.cpp
  DEPENDS
    myclean::platform_etw
    ws2_32)
