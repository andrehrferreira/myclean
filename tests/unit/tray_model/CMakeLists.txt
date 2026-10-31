# The tray's view models over a scripted fake service: formats, icon state and pixels, tooltip,
# chart buffers, reply parsing, polling, the incident queue and the dashboard model.
myclean_add_test(tray_model
  SOURCES
    chart_test.cpp
    dashboard_model_test.cpp
    format_test.cpp
    incident_queue_test.cpp
    poller_test.cpp
    replies_test.cpp
    tray_model_test_support.hpp
    tray_state_test.cpp
  DEPENDS
    myclean::tray_model)
