# The tray's view models, without any UI or Win32: polling the service, the live and past-range
# chart buffers, alerts and their toasts, the governor's status, owner choices and toasts, the
# approval inbox and Space lists with their treemap, the freeze watchdog's incident queue, the
# tray icon's state and pixels, the tooltip and the text formats of the dashboard.
myclean_add_library(tray_model
  SOURCES
    audit.cpp
    audit.hpp
    chart.cpp
    chart.hpp
    cleaning.cpp
    cleaning.hpp
    dashboard_model.cpp
    dashboard_model.hpp
    format.cpp
    format.hpp
    governor.cpp
    governor.hpp
    incident_queue.cpp
    incident_queue.hpp
    inventory.cpp
    inventory.hpp
    json_read.hpp
    optimizer.cpp
    optimizer.hpp
    poller.cpp
    poller.hpp
    replies.cpp
    replies.hpp
    reports.cpp
    reports.hpp
    screens.cpp
    screens.hpp
    service_link.hpp
    snapshot.cpp
    snapshot.hpp
    space_map.cpp
    space_map.hpp
    tray_state.cpp
    tray_state.hpp
  DEPENDS
    myclean::input
    myclean::ipc
    myclean::model
    myclean::space
    nlohmann_json::nlohmann_json)
