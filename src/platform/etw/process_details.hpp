#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/events/events.hpp"

namespace myclean::platform::etw {

// Maps NT device paths ("\Device\HarddiskVolume3\Windows\notepad.exe") to drive-letter paths
// ("C:\Windows\notepad.exe"), UTF-8. The map of drive letters is read on first use and re-read,
// at most once a minute, when a device path has no drive letter (a volume mounted since). Not
// thread-safe.
class DevicePaths {
 public:
  // `nt_path` with its device replaced by its drive letter; unchanged when no drive letter maps
  // to the device (a volume mounted in a folder, a network path) or it is not a device path.
  [[nodiscard]] std::string to_dos(std::string_view nt_path);

 private:
  [[nodiscard]] std::optional<std::string> mapped(std::string_view nt_path) const;
  void refresh();

  std::vector<std::pair<std::string, std::string>> devices_;  // "\Device\HarddiskVolume3", "C:"
  std::optional<std::chrono::steady_clock::time_point> refreshed_;
};

// The command line of `process`, UTF-8, read only while the process with that PID still has that
// creation time, so a reused PID never gives another process's command line. nullopt when the
// process exited or denies access (non-elevated callers can read only their own user's
// processes). Uses NtQueryInformationProcess(ProcessCommandLineInformation), resolved from ntdll
// at run time like platform/proc.
[[nodiscard]] std::optional<std::string> command_line_of(const events::ProcessKey& process);

}  // namespace myclean::platform::etw
