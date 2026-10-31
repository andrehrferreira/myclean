# The tray's view models, without any UI or Win32: polling the service, the live and past-range
# chart buffers, alerts and their toasts, the approval inbox and Space lists, the freeze
# watchdog's incident queue, the tray icon's state and pixels, the tooltip and the text formats
# of the dashboard.
myclean_add_library(tray_model
  SOURCES
    chart.cpp
    chart.hpp
    cleaning.cpp
    cleaning.hpp
    dashboard_model.cpp
    dashboard_model.hpp
    format.cpp
    format.hpp
    incident_queue.cpp
    incident_queue.hpp
    poller.cpp
    poller.hpp
    replies.cpp
    replies.hpp
    service_link.hpp
    snapshot.cpp
    snapshot.hpp
    tray_state.cpp
    tray_state.hpp
  DEPENDS
    myclean::input
    myclean::ipc
    myclean::model
    nlohmann_json::nlohmann_json)
