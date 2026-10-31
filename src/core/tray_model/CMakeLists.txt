# The tray's view models, without any UI or Win32: polling the service, the live chart buffers,
# the tray icon's state and pixels, the tooltip and the text formats of the dashboard.
myclean_add_library(tray_model
  SOURCES
    chart.cpp
    chart.hpp
    dashboard_model.cpp
    dashboard_model.hpp
    format.cpp
    format.hpp
    poller.cpp
    poller.hpp
    replies.cpp
    replies.hpp
    snapshot.cpp
    snapshot.hpp
    tray_state.cpp
    tray_state.hpp
  DEPENDS
    myclean::ipc
    myclean::model
    nlohmann_json::nlohmann_json)
