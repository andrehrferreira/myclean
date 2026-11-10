# The service host: myclean-svc.exe and the library behind it (lifecycle, components, IPC
# methods), which the tests drive in-process.
myclean_add_library(svc
  SOURCES
    alerts_component.cpp
    alerts_component.hpp
    cleaning_component.cpp
    cleaning_component.hpp
    component.hpp
    disk_health_component.cpp
    disk_health_component.hpp
    events_adapter.cpp
    events_adapter.hpp
    governor_component.cpp
    governor_component.hpp
    host.cpp
    host.hpp
    input_component.cpp
    input_component.hpp
    owner_session.cpp
    owner_session.hpp
    service.cpp
    service.hpp
    startup_component.cpp
    startup_component.hpp
    telemetry_component.cpp
    telemetry_component.hpp
    trust_component.cpp
    trust_component.hpp
  DEPENDS
    myclean::actions
    myclean::alerts
    myclean::cleaning
    myclean::events
    myclean::governing
    myclean::health
    myclean::input
    myclean::ipc
    myclean::os
    myclean::packs
    myclean::platform_clean
    myclean::platform_fs
    myclean::platform_git
    myclean::platform_ipc
    myclean::platform_proc
    myclean::platform_scan
    myclean::platform_service
    myclean::platform_settings
    myclean::platform_startup
    myclean::rules
    myclean::startup
    myclean::store
    myclean::telemetry
    myclean::trust
    spdlog::spdlog
    userenv
    wtsapi32)
target_compile_definitions(myclean_svc PRIVATE MYCLEAN_VERSION="${PROJECT_VERSION}")

add_executable(myclean-svc main.cpp ${PROJECT_SOURCE_DIR}/src/resources/myclean.rc)
target_link_libraries(myclean-svc PRIVATE
  myclean::svc myclean::platform_cleanup myclean::platform_diagnostics myclean::platform_disk
  myclean::platform_disk_health myclean::platform_etw myclean::platform_games myclean::platform_git
  myclean::platform_jobs myclean::platform_packs myclean::platform_perf myclean::platform_proc
  myclean::platform_settings myclean::platform_trust)
if(MYCLEAN_ASAN)
  add_custom_command(TARGET myclean-svc POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MYCLEAN_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:myclean-svc>"
    VERBATIM)
endif()
myclean_install_executable(myclean-svc)
_myclean_register_lint_sources(main.cpp)
