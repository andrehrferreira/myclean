# Process start/stop and per-process network events: the event model and source interface, the
# router that resolves parents safely against PID reuse, persistence (proc_start, net_minute)
# and the service component. The ETW source and the TCP/UDP tables live in src/platform/etw.
myclean_add_library(events
  SOURCES
    connections.hpp
    event_router.cpp
    event_router.hpp
    event_store.cpp
    event_store.hpp
    events.cpp
    events.hpp
    events_component.cpp
    events_component.hpp
  DEPENDS
    myclean::model
    myclean::os
    myclean::store
    myclean::telemetry)
