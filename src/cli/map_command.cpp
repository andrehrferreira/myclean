#include "cli/map_command.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "cli/cli.hpp"
#include "cli/packs_command.hpp"
#include "cli/text.hpp"
#include "core/clean/workspaces.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/map/json.hpp"
#include "core/map/project_areas.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/space/scanner.hpp"
#include "platform/clean/win32_probes.hpp"
#include "platform/scan/directory_reader.hpp"

namespace myclean::cli {

namespace {

namespace fs = std::filesystem;

constexpr double gib = 1024.0 * 1024.0 * 1024.0;

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// Lowers this process to below-normal CPU priority and background I/O and memory priority for
// its lifetime, then restores the previous priority class.
class BackgroundPriority {
 public:
  BackgroundPriority()
      : previous_(::GetPriorityClass(::GetCurrentProcess())), background_(enter_background()) {}
  ~BackgroundPriority() {
    if (background_) {
      static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_END));
    }
    if (previous_ != 0) {
      static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), previous_));
    }
  }
  BackgroundPriority(const BackgroundPriority&) = delete;
  BackgroundPriority& operator=(const BackgroundPriority&) = delete;
  BackgroundPriority(BackgroundPriority&&) = delete;
  BackgroundPriority& operator=(BackgroundPriority&&) = delete;

 private:
  // Background mode, then below-normal CPU priority: entering background mode resets the
  // priority class to normal, and a later class change keeps background mode. True when
  // background mode began.
  static bool enter_background() {
    const bool background =
        ::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN) != FALSE;
    static_cast<void>(::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS));
    return background;
  }

  DWORD previous_;
  bool background_;
};

// `path` with its 8.3 short names spelled out (C:\Users\RUNNER~1 is C:\Users\runneradmin), as
// the attribution areas spell their folders; `path` itself when it cannot be resolved.
fs::path long_path(const fs::path& path) {
  const DWORD needed = ::GetLongPathNameW(path.c_str(), nullptr, 0);
  if (needed == 0) {
    return path;
  }
  std::wstring buffer(needed, L'\0');
  const DWORD length = ::GetLongPathNameW(path.c_str(), buffer.data(), needed);
  if (length == 0 || length >= needed) {
    return path;
  }
  buffer.resize(length);
  return fs::path{std::move(buffer)};
}

// The folder to scan: `target` made absolute ("C:" is the volume "C:\") with its short names
// spelled out, or the system drive.
Result<fs::path> scan_root(const std::optional<std::string>& target) {
  fs::path path;
  if (target) {
    path = fs::path{std::u8string(target->begin(), target->end())};
  } else {
    std::array<wchar_t, 8> drive{};
    const DWORD length =
        ::GetEnvironmentVariableW(L"SystemDrive", drive.data(), static_cast<DWORD>(drive.size()));
    path = length > 0 && length < drive.size() ? fs::path{drive.data()} : fs::path{L"C:"};
  }
  if (path.has_root_name() && !path.has_root_directory() && !path.has_relative_path()) {
    path /= L"\\";
  }
  std::error_code error;
  path = fs::absolute(path, error).lexically_normal();
  if (error) {
    return fail(Errc::invalid_argument, "cannot resolve the path: " + error.message());
  }
  if (!path.has_filename() && path.has_relative_path()) {
    path = path.parent_path();
  }
  return long_path(path);
}

// The folder names the artifact scanner never enters (data_dir\workspaces.toml), or
// default_skip() without a readable file.
std::vector<std::wstring> workspace_skip(const fs::path& data_dir) {
  const rules::DiskDataFiles files;
  const fs::path file = data_dir / "workspaces.toml";
  const Result<std::string> text = files.read(file);
  const Result<clean::Workspaces> parsed =
      text ? clean::parse_workspace_file(*text, file) : tl::unexpected(text.error());
  return parsed ? parsed->skip : default_skip();
}

std::optional<map::VolumeSpace> volume_space(const fs::path& root) {
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free{};
  const std::wstring volume = root.root_path().native();
  if (::GetDiskFreeSpaceExW(volume.c_str(), nullptr, &total, &free) == FALSE) {
    return std::nullopt;
  }
  return map::VolumeSpace{.total = Bytes{total.QuadPart}, .free = Bytes{free.QuadPart}};
}

}  // namespace

fs::path default_data_dir() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length < buffer.size()) {
      return fs::path{std::wstring(buffer.data(), length)}.parent_path().parent_path() / "data";
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::vector<std::wstring> default_skip() {
  return {L".git", L"node_modules"};
}

int map_command(const MapRequest& request, std::ostream& out, std::ostream& err) {
  const Result<fs::path> root = scan_root(request.target);
  if (!root) {
    err << "myclean: map: " << root.error().message << '\n';
    return exit_usage;
  }
  const fs::path data_dir = request.data_dir.value_or(default_data_dir());
  LocalPacks local = load_local_packs(data_dir);
  for (const Error& error : local.report.errors) {
    err << "myclean: map: " << error.message << '\n';
  }
  if (local.report.packs.empty()) {
    err << "myclean: map: no knowledge pack loaded from " << utf8(data_dir / "packs") << '\n';
    return exit_unavailable;
  }

  const platform::Win32DirectoryReader reader;
  Result<space::ScanResult> scan = [&] {
    const BackgroundPriority priority;
    return space::scan(reader, *root, {.workers = 4, .progress_every = 50'000}, {},
                       [&err](const space::ScanProgress& progress) {
                         err << std::format(
                                    "\rscanning: {} folders, {} entries, {:.1f} GiB",
                                    progress.folders, progress.files,
                                    static_cast<double>(progress.size_on_disk.count()) / gib)
                             << std::flush;
                       });
  }();
  err << "\n";
  if (!scan) {
    err << "myclean: map: " << scan.error().message << '\n';
    return exit_unavailable;
  }

  const platform::Win32Environment environment;
  const map::MapOptions options{
      .min_size = Bytes{static_cast<std::uint64_t>(std::llround(request.min_gb * gib))},
      .depth = request.depth};
  // The artifact folders of the projects in the scan explain what no fixed location names.
  std::vector<map::Area> areas = std::move(local.registry.attribution);
  std::ranges::move(map::project_areas(*scan, local.registry.clean, workspace_skip(data_dir)),
                    std::back_inserter(areas));
  const Result<map::DiskMap> disk_map =
      map::build_map(*scan, std::move(areas), environment, volume_space(*root), options);
  if (!disk_map) {
    err << "myclean: map: " << disk_map.error().message << '\n';
    return exit_usage;
  }
  const nlohmann::json json = *disk_map;
  out << (request.json ? json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n"
                       : text::map(json));
  return exit_ok;
}

}  // namespace myclean::cli
