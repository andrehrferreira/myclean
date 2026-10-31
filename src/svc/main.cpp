// myclean-svc.exe: the MyClean service host. See svc/host.hpp for the command line.

#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/service/scm.hpp"
#include "svc/component.hpp"
#include "svc/host.hpp"

namespace {

using myclean::Result;
namespace svc = myclean::svc;

HANDLE& console_stop_event() {
  static HANDLE event = nullptr;
  return event;
}

BOOL WINAPI on_console_control(DWORD control) {
  if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT) {
    ::SetEvent(console_stop_event());
    return TRUE;
  }
  return FALSE;
}

// Every component the service runs, in start order. Collectors are registered here.
std::vector<std::unique_ptr<svc::IComponent>> make_components() {
  return {};
}

int run_console(const svc::HostOptions& options) {
  Result<wil::unique_event_nothrow> stop = myclean::platform::pipe_io::make_event();
  if (!stop) {
    std::cerr << "myclean-svc: " << stop.error().message << '\n';
    return svc::exit_start_failed;
  }
  console_stop_event() = stop->get();
  ::SetConsoleCtrlHandler(&on_console_control, TRUE);
  return svc::run(options, make_components(), stop->get(),
                  [] { std::cerr << "myclean-svc: running; press Ctrl+C to stop\n"; });
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program entry point
int wmain(int argc, wchar_t** argv) {
  const std::span<wchar_t*> raw(argv, static_cast<std::size_t>(argc));
  const std::vector<std::wstring> args(raw.begin() + 1, raw.end());
  const Result<svc::HostOptions> options = svc::parse_command_line(args);
  if (!options) {
    std::cerr << "myclean-svc: " << options.error().message << '\n' << svc::usage();
    return svc::exit_start_failed;
  }
  if (options->mode == svc::Mode::console) {
    return run_console(*options);
  }
  const Result<int> code = myclean::platform::run_service(
      std::wstring(svc::service_name),
      [&options](HANDLE stop_event, const std::function<void()>& report_running) {
        return svc::run(*options, make_components(), stop_event, report_running);
      });
  if (!code) {
    std::cerr << "myclean-svc: " << code.error().message << '\n' << svc::usage();
    return svc::exit_start_failed;
  }
  return *code;
}
