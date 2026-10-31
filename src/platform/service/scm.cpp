#include "platform/service/scm.hpp"

#include <array>
#include <mutex>
#include <string>
#include <utility>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"

namespace myclean::platform {

namespace {

// Start-up includes the integrity check of the store, which grows with the database.
constexpr DWORD start_wait_hint_ms = 30'000;

class ServiceState {
 public:
  ServiceState(std::wstring name, ServiceBody body, wil::unique_event_nothrow stop)
      : name_(std::move(name)), body_(std::move(body)), stop_(std::move(stop)) {}

  [[nodiscard]] wchar_t* name() { return name_.data(); }
  [[nodiscard]] int exit_code() const { return exit_code_; }

  // ServiceMain.
  void main() {
    handle_ = ::RegisterServiceCtrlHandlerExW(name_.c_str(), &ServiceState::handler, this);
    if (handle_ == nullptr) {
      exit_code_ = 1;
      return;
    }
    set_status(SERVICE_START_PENDING, start_wait_hint_ms);
    exit_code_ = body_(stop_.get(), [this] { set_status(SERVICE_RUNNING, 0); });
    set_status(SERVICE_STOPPED, 0, exit_code_);
  }

 private:
  static DWORD WINAPI handler(DWORD control, DWORD /*event_type*/, void* /*event_data*/,
                              void* context) {
    auto* self = static_cast<ServiceState*>(context);
    switch (control) {
      case SERVICE_CONTROL_STOP:
      case SERVICE_CONTROL_PRESHUTDOWN:
        self->set_status(SERVICE_STOP_PENDING, stop_wait_hint_ms);
        self->stop_.SetEvent();
        return NO_ERROR;
      case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
      default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
  }

  void set_status(DWORD state, DWORD wait_hint_ms, int exit_code = 0) {
    const std::scoped_lock lock(mutex_);
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN : 0;
    status.dwWin32ExitCode = exit_code == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = static_cast<DWORD>(exit_code);
    status.dwCheckPoint = pending ? ++checkpoint_ : 0;
    status.dwWaitHint = wait_hint_ms;
    ::SetServiceStatus(handle_, &status);
  }

  std::wstring name_;
  ServiceBody body_;
  wil::unique_event_nothrow stop_;
  SERVICE_STATUS_HANDLE handle_ = nullptr;
  std::mutex mutex_;  // serializes SetServiceStatus between the handler and the body
  DWORD checkpoint_ = 0;
  int exit_code_ = 0;
};

// ServiceMain receives no context, so the one service of the process is reached through this.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): see above
ServiceState* current_service = nullptr;

void WINAPI service_main(DWORD /*argc*/, wchar_t** /*argv*/) {
  current_service->main();
}

}  // namespace

Result<int> run_service(std::wstring name, ServiceBody body) {
  Result<wil::unique_event_nothrow> stop = pipe_io::make_event();
  if (!stop) {
    return tl::unexpected(stop.error());
  }
  ServiceState service(std::move(name), std::move(body), std::move(*stop));
  current_service = &service;
  std::array<SERVICE_TABLE_ENTRYW, 2> table{
      {{.lpServiceName = service.name(), .lpServiceProc = &service_main},
       {.lpServiceName = nullptr, .lpServiceProc = nullptr}}};
  const BOOL dispatched = ::StartServiceCtrlDispatcherW(table.data());
  const DWORD error = ::GetLastError();
  current_service = nullptr;
  if (dispatched == FALSE) {
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      return fail(Errc::invalid_state, "not started by the service control manager");
    }
    return pipe_io::failure("StartServiceCtrlDispatcher", error);
  }
  return service.exit_code();
}

}  // namespace myclean::platform
