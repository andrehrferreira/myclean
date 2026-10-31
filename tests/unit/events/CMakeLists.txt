# Event router, persistence and the events component against in-memory fakes of the event
# source, connection table and process table.
myclean_add_test(events
  SOURCES
    event_fakes.hpp
    event_router_test.cpp
    event_store_test.cpp
    events_component_test.cpp
    events_test.cpp
  DEPENDS
    myclean::events
    myclean::fakes)
