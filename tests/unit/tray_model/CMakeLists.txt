# The tray's view models over a scripted fake service: formats, icon state and pixels, tooltip,
# chart buffers, reply parsing, polling, the incident queue and the dashboard model, and the
# approval inbox: its replies, its requests and the user tasks driven through the poller, and the
# governor's status, requests and toasts.
myclean_add_test(tray_model
  SOURCES
    chart_test.cpp
    cleaning_test.cpp
    dashboard_model_test.cpp
    format_test.cpp
    governor_test.cpp
    inbox_fixtures.hpp
    inbox_poller_test.cpp
    incident_queue_test.cpp
    poller_test.cpp
    replies_test.cpp
    tray_model_test_support.hpp
    tray_state_test.cpp
  DEPENDS
    myclean::tray_model)
