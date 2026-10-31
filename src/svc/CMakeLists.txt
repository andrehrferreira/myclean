# The service host: myclean-svc.exe and the library behind it (lifecycle, components, IPC
# methods), which the tests drive in-process.
myclean_add_library(svc
  SOURCES
    component.hpp
    host.cpp
    host.hpp
    service.cpp
    service.hpp
    telemetry_component.cpp
    telemetry_component.hpp
  DEPENDS
    myclean::ipc
    myclean::os
    myclean::platform_ipc
    myclean::platform_service
    myclean::store
    myclean::telemetry
    spdlog::spdlog)
target_compile_definitions(myclean_svc PRIVATE MYCLEAN_VERSION="${PROJECT_VERSION}")

add_executable(myclean-svc main.cpp)
target_link_libraries(myclean-svc PRIVATE
  myclean::svc myclean::platform_disk myclean::platform_perf myclean::platform_proc)
if(MYCLEAN_ASAN)
  add_custom_command(TARGET myclean-svc POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MYCLEAN_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:myclean-svc>"
    VERBATIM)
endif()
myclean_install_executable(myclean-svc)
_myclean_register_lint_sources(main.cpp)
