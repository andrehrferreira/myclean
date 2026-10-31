# The tray app: myclean-tray.exe and the library behind it (single instance and quit event, the
# pipe link and poll thread, the icon and its toasts, the Dear ImGui dashboard, the hosting of the
# freeze watchdog), which the tests drive headless.
myclean_add_library(tray
  SOURCES
    app.cpp
    app.hpp
    dashboard_ui.cpp
    dashboard_ui.hpp
    dashboard_window.cpp
    dashboard_window.hpp
    icon.cpp
    icon.hpp
    instance.cpp
    instance.hpp
    pipe_link.cpp
    pipe_link.hpp
    poll_thread.cpp
    poll_thread.hpp
    tray_icon.cpp
    tray_icon.hpp
    window_object.hpp
  DEPENDS
    myclean::os
    myclean::platform_input
    myclean::platform_ipc
    myclean::platform_service
    myclean::tray_model
    imgui::imgui
    implot::implot
    spdlog::spdlog
    WIL::WIL
    d3d11
    d3dcompiler
    dwmapi
    dxgi
    gdi32
    shell32
    user32)

add_executable(myclean-tray WIN32 main.cpp myclean-tray.manifest)
target_link_libraries(myclean-tray PRIVATE myclean::tray)
if(MYCLEAN_ASAN)
  add_custom_command(TARGET myclean-tray POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MYCLEAN_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:myclean-tray>"
    VERBATIM)
endif()
myclean_install_executable(myclean-tray)
_myclean_register_lint_sources(main.cpp)
