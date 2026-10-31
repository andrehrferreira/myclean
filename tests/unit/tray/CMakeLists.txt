# The tray app's library, headless: single instance and quit event (test-only names), icons
# (GDI objects only), the pipe link against an in-process service on a unique pipe name, the
# poll thread, and the dashboard drawn by Dear ImGui without a window or renderer. Nothing here
# shows a window, an icon or a notification.
myclean_add_test(tray
  SOURCES
    dashboard_ui_test.cpp
    icon_test.cpp
    instance_test.cpp
    pipe_link_test.cpp
    poll_thread_test.cpp
  DEPENDS
    myclean::tray
    myclean::fakes)
