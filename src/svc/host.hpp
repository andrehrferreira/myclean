#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "core/model/result.hpp"
#include "platform/ipc/pipe_server.hpp"
#include "svc/component.hpp"

namespace myclean::svc {

// The service's SCM name (contract C3).
inline constexpr std::wstring_view service_name = L"MyClean";

enum class Mode {
  service,  // started by the SCM; state in %ProgramData%\MyClean
  console,  // started by hand for development; state in --state or %LOCALAPPDATA%\MyClean\console
};

struct HostOptions {
  Mode mode = Mode::service;
  std::optional<std::filesystem::path> state_dir;  // console mode only
  std::wstring pipe_name{platform::default_pipe_name};
  // Console mode only: the tray executable allowed to call the tray-only methods (default: the
  // myclean-tray.exe next to myclean-svc.exe), and the cleaning sandbox (see CleaningMode).
  std::optional<std::filesystem::path> tray_image;
  std::optional<std::filesystem::path> sandbox;
};

// The command line after the program name:
//   (nothing)                                     run as the MyClean service under the SCM
//   --console [--state <dir>] [--pipe <name>] [--tray <exe>] [--sandbox <dir>]
//                                                 run in this console until Ctrl+C or Ctrl+Break
// <name> is the pipe name after \\.\pipe\ and must be a token (see ipc::is_token).
[[nodiscard]] Result<HostOptions> parse_command_line(std::span<const std::wstring> args);
[[nodiscard]] std::string_view usage();

// Where a build keeps its read-only files, from its executable: <version>\bin\myclean-svc.exe
// reads <version>\data and <version>\manifest.json (contract C4).
struct InstallLayout {
  std::filesystem::path data_dir;
  std::filesystem::path manifest;
};
[[nodiscard]] InstallLayout install_layout(const std::filesystem::path& exe);

// The "version" of a deploy manifest; none when the file is missing, unreadable or has none.
[[nodiscard]] std::optional<std::string> manifest_version(const std::filesystem::path& manifest);

// The owner's SID, recorded in <state_dir>\owner.sid on the first service start: the user logged
// on at the console then. The file is trusted only when it belongs to one of `trusted_owners`
// (SYSTEM or Administrators: the only accounts that can write the protected state folder), so a
// file planted by another user before the first start is refused, not used.
[[nodiscard]] Result<std::string> resolve_owner(const std::filesystem::path& state_dir,
                                                std::span<const std::string_view> trusted_owners);

// Exit codes of run().
inline constexpr int exit_ok = 0;
inline constexpr int exit_start_failed = 1;
inline constexpr int exit_stop_failed = 2;  // the final flush failed

// The whole service life: locates the install and state folders, opens the log
// (<state>\logs\myclean-svc.log, rotating), secures the state folder and records crash dumps
// (service mode), starts Service with `components`, calls `report_running`, ticks it every second
// until `stop_event` is signaled, and stops it. Every start-up failure is logged and ends the
// run with exit_start_failed, so the service stops instead of running half-initialized (C7).
[[nodiscard]] int run(const HostOptions& options,
                      std::vector<std::unique_ptr<IComponent>> components, HANDLE stop_event,
                      const std::function<void()>& report_running);

}  // namespace myclean::svc
