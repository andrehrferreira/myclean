// myclean-svc.exe: the MyClean service host. See svc/host.hpp for the command line.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/disk_health/win32_disk_health.hpp"
#include "platform/etw/etw_event_source.hpp"
#include "platform/etw/ip_helper_connections.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/perf/pdh_counters.hpp"
#include "platform/perf/win32_memory_status.hpp"
#include "platform/proc/nt_process_table.hpp"
#include "platform/service/scm.hpp"
#include "platform/trust/win_image_inspector.hpp"
#include "svc/alerts_component.hpp"
#include "svc/component.hpp"
#include "svc/disk_health_component.hpp"
#include "svc/events_adapter.hpp"
#include "svc/host.hpp"
#include "svc/input_component.hpp"
#include "svc/telemetry_component.hpp"
#include "svc/trust_component.hpp"

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

// The drive Windows runs from, "C:", as a volume series entity; empty when unknown.
std::string system_drive() {
  std::array<wchar_t, MAX_PATH> windows{};
  const UINT length = ::GetSystemWindowsDirectoryW(windows.data(), MAX_PATH);
  if (length < 2 || length >= MAX_PATH || windows[1] != L':') {
    return {};
  }
  return {static_cast<char>(windows[0]), ':'};
}

// Every component the service runs, in start order: a component starts after those it uses and
// stops before them. Collectors are registered here.
std::vector<std::unique_ptr<svc::IComponent>> make_components() {
  namespace platform = myclean::platform;
  static const myclean::rules::DiskDataFiles files;
  auto alerts = std::make_unique<svc::AlertsComponent>(files);
  auto trust = std::make_unique<svc::TrustComponent>(
      svc::TrustSources{.inspector = std::make_unique<platform::WinImageInspector>(),
                        .images = std::make_unique<platform::WinProcessImages>(),
                        .user_writable_roots = platform::user_writable_roots()},
      files);
  auto telemetry = std::make_unique<svc::TelemetryComponent>(
      svc::TelemetrySources{.processes = std::make_unique<platform::NtProcessTable>(),
                            .counters = std::make_unique<platform::PdhCounters>(),
                            .memory = std::make_unique<platform::Win32MemoryStatus>(),
                            .volumes = std::make_unique<platform::Win32VolumeInfo>(),
                            .system_drive = system_drive()},
      [trust = trust.get()](std::uint32_t pid, myclean::WallTime created) {
        return trust->process(pid, created);
      },
      alerts.get());
  auto disk_health = std::make_unique<svc::DiskHealthComponent>(
      std::make_unique<platform::Win32DiskHealthSource>(), files, *alerts,
      [telemetry = telemetry.get()] { return telemetry->volumes(); });
  auto events = std::make_unique<svc::EventsAdapter>(
      svc::EventSources{.events = std::make_unique<platform::etw::EtwEventSource>(),
                        .connections = std::make_unique<platform::etw::IpHelperConnectionTable>(),
                        .processes = std::make_unique<platform::NtProcessTable>()},
      trust.get());

  std::vector<std::unique_ptr<svc::IComponent>> components;
  components.push_back(std::move(alerts));
  components.push_back(std::move(trust));
  components.push_back(std::move(telemetry));
  components.push_back(std::move(disk_health));
  components.push_back(std::move(events));
  components.push_back(std::make_unique<svc::InputComponent>());
  return components;
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
