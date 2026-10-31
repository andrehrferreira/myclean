#pragma once

#include <functional>
#include <string>

#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {

// The body of a service: runs until `stop_event` is signaled (SCM stop or pre-shutdown), calls
// `report_running` once start-up has succeeded, and returns the exit code (0: clean stop).
using ServiceBody =
    std::function<int(HANDLE stop_event, const std::function<void()>& report_running)>;

// The SCM's wait hint for a stop: the service promises SERVICE_STOPPED within 30 s (contract C3).
inline constexpr DWORD stop_wait_hint_ms = 25'000;

// Runs this process as the own-process service `name`: connects to the SCM, reports
// SERVICE_START_PENDING, runs `body`, and reports SERVICE_STOPPED with its exit code. A non-zero
// code is reported as a service-specific error, which the SCM records as a failure. Stop and
// pre-shutdown are accepted only while running. Returns the body's exit code; fails with
// invalid_state when the process was not started by the SCM. One service per process.
[[nodiscard]] Result<int> run_service(std::wstring name, ServiceBody body);

}  // namespace myclean::platform
