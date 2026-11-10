// myclean-svc.exe: the MyClean service host. See svc/host.hpp for the command line.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/events/event_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/governing/governor.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/loader.hpp"
#include "core/placement/drives.hpp"
#include "core/telemetry/metrics.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/cleanup/win32_command_runner.hpp"
#include "platform/cleanup/win32_handler_host.hpp"
#include "platform/cleanup/win32_recycle_bin.hpp"
#include "platform/diagnostics/win32_diagnostics_log.hpp"
#include "platform/disk/win32_volume_info.hpp"
#include "platform/disk_health/win32_disk_health.hpp"
#include "platform/etw/etw_event_source.hpp"
#include "platform/etw/ip_helper_connections.hpp"
#include "platform/etw/process_details.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "platform/games/win32_games.hpp"
#include "platform/git/git_cli.hpp"
#include "platform/image/wic_image_decoder.hpp"
#include "platform/indexer/win32_indexer_scope.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/jobs/win32_jobs.hpp"
#include "platform/jobs/win32_process_control.hpp"
#include "platform/packs/win32_packs.hpp"
#include "platform/perf/pdh_counters.hpp"
#include "platform/perf/win32_memory_status.hpp"
#include "platform/proc/nt_process_table.hpp"
#include "platform/scan/directory_reader.hpp"
#include "platform/scan/file_content.hpp"
#include "platform/scan/installer_inspector.hpp"
#include "platform/service/scm.hpp"
#include "platform/settings/process_runner.hpp"
#include "platform/settings/seek_penalty.hpp"
#include "platform/settings/system_space.hpp"
#include "platform/startup/win32_startup.hpp"
#include "platform/trust/win_image_inspector.hpp"
#include "svc/alerts_component.hpp"
#include "svc/cleaning_component.hpp"
#include "svc/component.hpp"
#include "svc/disk_health_component.hpp"
#include "svc/events_adapter.hpp"
#include "svc/governor_component.hpp"
#include "svc/host.hpp"
#include "svc/input_component.hpp"
#include "svc/owner_session.hpp"
#include "svc/resident_component.hpp"
#include "svc/startup_component.hpp"
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

constexpr std::chrono::minutes handler_scan_timeout{2};
constexpr std::chrono::minutes handler_purge_timeout{30};
// Below this share of idle time the system volume's disk is critically busy: no auto run.
constexpr double busy_idle_percent = 10.0;

// The volumes as relocation and quarantine targets, with their latest disk health.
class DriveFacts final : public myclean::placement::IDriveFacts {
 public:
  explicit DriveFacts(myclean::placement::LiveDriveFacts::HealthLatest health)
      : facts_(volumes_, seek_penalty_, std::move(health)) {}

  [[nodiscard]] Result<std::vector<myclean::placement::TargetVolume>> volumes() const override {
    return facts_.volumes();
  }

 private:
  myclean::platform::Win32VolumeInfo volumes_;
  myclean::platform::Win32SeekPenalty seek_penalty_;
  myclean::placement::LiveDriveFacts facts_;
};

// True while a disk of the system volume is idle less than busy_idle_percent of the time.
bool system_volume_busy(const svc::TelemetryComponent& telemetry, const std::string& drive) {
  const std::shared_ptr<const myclean::telemetry::VolumeView> view = telemetry.volumes();
  if (!view || drive.empty()) {
    return false;
  }
  for (const myclean::telemetry::VolumeStatus& volume : view->volumes) {
    if (std::ranges::find(volume.mount_points, drive + "\\") == volume.mount_points.end()) {
      continue;
    }
    for (const std::uint32_t disk : volume.disks) {
      const std::optional<double> idle = telemetry.latest_value("disk.idle", std::to_string(disk));
      if (idle && *idle < busy_idle_percent) {
        return true;
      }
    }
  }
  return false;
}

// The owner's override packs folder, when administrators alone can write to it.
myclean::Result<std::optional<std::filesystem::path>> owner_packs() {
  namespace platform = myclean::platform;
  return platform::owner_packs_dir().and_then(platform::owner_packs_folder);
}

std::unique_ptr<svc::CleaningComponent> make_cleaning(const svc::HostOptions& options,
                                                      const myclean::packs::IPackFiles& files,
                                                      const svc::TelemetryComponent& telemetry,
                                                      const svc::DiskHealthComponent& disks) {
  namespace platform = myclean::platform;
  const std::string drive = system_drive();
  const std::optional<std::filesystem::path> git = platform::find_git();
  svc::CleaningSources sources{
      .file_system = std::make_unique<platform::Win32FileSystem>(),
      .tree = std::make_unique<platform::Win32FileTree>(),
      .usage = std::make_unique<platform::Win32Usage>(),
      .git = git ? std::make_unique<platform::GitCli>(*git) : nullptr,
      .handlers =
          std::make_unique<platform::Win32HandlerHost>(handler_scan_timeout, handler_purge_timeout),
      .commands = std::make_unique<platform::Win32CommandRunner>(),
      .dism = platform::dism_path(),
      .system = std::make_unique<platform::Win32SystemSpace>(),
      .tools = std::make_unique<platform::Win32ProcessRunner>(),
      .drives = std::make_unique<DriveFacts>([&disks] { return disks.latest(); }),
      .reader = std::make_unique<platform::Win32DirectoryReader>(),
      .zones = std::make_unique<platform::Win32ZoneIdentifierReader>(),
      .hasher = std::make_unique<platform::Win32ContentHasher>(),
      .installers = std::make_unique<platform::Win32InstallerInspector>(),
      .images = std::make_unique<platform::WicImageDecoder>(),
      .owner = [](std::string_view owner) { return svc::owner_context(owner); },
      // NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc, as anywhere in the service
      .busy = [&telemetry, drive] { return system_volume_busy(telemetry, drive); },
      .machine = [](std::string_view owner) -> std::unique_ptr<myclean::packs::IMachine> {
        return std::make_unique<platform::Win32Machine>(std::wstring(owner.begin(), owner.end()));
      },
      .owner_packs = owner_packs,
      .bin = [](std::string_view owner) -> std::unique_ptr<myclean::cleanup::IBinRecords> {
        return std::make_unique<platform::Win32BinRecords>(
            std::wstring(owner.begin(), owner.end()));
      },
      .startup = [](std::string_view owner) -> std::unique_ptr<myclean::startup::IStartupSystem> {
        return std::make_unique<platform::Win32StartupSystem>(std::string(owner));
      },
      .diagnostics = std::make_unique<platform::Win32DiagnosticsLog>(),
      .indexer = std::make_unique<platform::Win32IndexerScope>()};
  // "C:" / "" stays "C:" (a drive-relative path); the handlers need the volume root "C:\".
  const std::filesystem::path root =
      std::filesystem::path((drive.empty() ? std::string("C:") : drive) + "\\");
  return std::make_unique<svc::CleaningComponent>(
      std::move(sources), files,
      svc::CleaningMode{.console = options.mode == svc::Mode::console,
                        .sandbox = options.sandbox,
                        .system_root = root});
}

// Game detection's sources as the owner sees them: launcher library roots (no owner roots yet)
// and the owner's GameConfigStore, read from HKU\<owner SID>.
myclean::governing::GameScan read_games(std::string_view owner_sid) {
  namespace platform = myclean::platform;
  const platform::Win32GameSources sources(std::wstring(owner_sid.begin(), owner_sid.end()));
  const platform::Win32GameFiles files;
  myclean::games::RootsReport roots =
      myclean::games::find_library_roots(sources.launcher_data(), {}, files);
  myclean::governing::GameScan scan{.sources = {.roots = std::move(roots.roots)},
                                    .problems = std::move(roots.problems)};
  if (Result<std::vector<myclean::games::GameConfigEntry>> entries = sources.game_config_store();
      entries) {
    scan.sources.game_config = std::move(*entries);
  } else {
    scan.problems.push_back("GameConfigStore: " + entries.error().message);
  }
  return scan;
}

// The governor: jobs owned by SYSTEM in the service; a console run opens only jobs its own user
// created.
std::unique_ptr<svc::GovernorComponent> make_governor(const svc::HostOptions& options,
                                                      const myclean::packs::IPackFiles& files,
                                                      const svc::TelemetryComponent& telemetry,
                                                      svc::EventsAdapter& events) {
  namespace platform = myclean::platform;
  std::string owner(platform::local_system_sid);
  if (options.mode == svc::Mode::console) {
    if (Result<std::string> user = platform::current_user_sid(); user) {
      owner = std::move(*user);
    }
  }
  auto images = std::make_shared<platform::WinProcessImages>();
  return std::make_unique<svc::GovernorComponent>(
      svc::GovernorSources{.jobs = std::make_unique<platform::Win32JobControl>(std::move(owner)),
                           .processes = std::make_unique<platform::Win32ProcessControl>(),
                           .gpu = std::make_unique<platform::PdhCounters>(),
                           .view = [&telemetry] { return telemetry.processes(); },
                           .metric =
                               [&telemetry](std::string_view metric) {
                                 return telemetry.latest_value(metric,
                                                               myclean::telemetry::metric::system);
                               },
                           .games = &read_games,
                           .image_path = [images](std::uint32_t pid, myclean::WallTime created)
                               -> std::optional<std::filesystem::path> {
                             Result<std::filesystem::path> path = images->image_path(pid, created);
                             return path ? std::optional(std::move(*path)) : std::nullopt;
                           },
                           .command_line =
                               [](std::uint32_t pid, myclean::WallTime created) {
                                 return platform::etw::command_line_of(
                                     myclean::events::ProcessKey{.pid = pid, .created = created});
                               },
                           .owner_packs = owner_packs},
      files, &events.router());
}

// The network bytes per process of the stored minutes in [from, to), for the resident history.
Result<std::vector<myclean::resident::ProcessTraffic>> network_traffic(myclean::store::Store& store,
                                                                       myclean::WallTime from,
                                                                       myclean::WallTime to) {
  Result<std::vector<myclean::events::ProcessNetwork>> totals =
      myclean::events::network_by_process(store, from, to);
  if (!totals) {
    return tl::unexpected(totals.error());
  }
  std::vector<myclean::resident::ProcessTraffic> traffic;
  traffic.reserve(totals->size());
  for (myclean::events::ProcessNetwork& process : *totals) {
    traffic.push_back({.pid = process.pid,
                       .created = process.created,
                       .image = std::move(process.image),
                       .bytes = process.sent + process.received});
  }
  return traffic;
}

// Every component the service runs, in start order: a component starts after those it uses and
// stops before them. Collectors are registered here.
std::vector<std::unique_ptr<svc::IComponent>> make_components(const svc::HostOptions& options) {
  namespace platform = myclean::platform;
  static const myclean::packs::DiskPackFiles files;
  auto alerts = std::make_unique<svc::AlertsComponent>(files);
  auto trust = std::make_unique<svc::TrustComponent>(
      svc::TrustSources{.inspector = std::make_unique<platform::WinImageInspector>(),
                        .images = std::make_unique<platform::WinProcessImages>(),
                        .user_writable_roots = platform::user_writable_roots(),
                        .owner_packs = owner_packs},
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

  auto governor = make_governor(options, files, *telemetry, *events);
  auto cleaning = make_cleaning(options, files, *telemetry, *disk_health);
  auto startup = std::make_unique<svc::StartupComponent>(svc::StartupSources{
      .view = [telemetry = telemetry.get()] { return telemetry->processes(); },
      .logon = [](std::string_view owner) { return svc::owner_logon_time(owner); },
      .diagnostics = std::make_unique<platform::Win32DiagnosticsLog>()});
  auto resident = std::make_unique<svc::ResidentComponent>(
      svc::ResidentSources{.view = [telemetry = telemetry.get()] { return telemetry->processes(); },
                           .network = network_traffic},
      alerts.get());

  std::vector<std::unique_ptr<svc::IComponent>> components;
  components.push_back(std::move(alerts));
  components.push_back(std::move(trust));
  components.push_back(std::move(telemetry));
  components.push_back(std::move(disk_health));
  components.push_back(std::move(events));
  components.push_back(std::move(governor));
  components.push_back(std::make_unique<svc::InputComponent>());
  components.push_back(std::move(startup));
  components.push_back(std::move(resident));
  components.push_back(std::move(cleaning));
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
  return svc::run(options, make_components(options), stop->get(),
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
        return svc::run(*options, make_components(*options), stop_event, report_running);
      });
  if (!code) {
    std::cerr << "myclean-svc: " << code.error().message << '\n' << svc::usage();
    return svc::exit_start_failed;
  }
  return *code;
}
