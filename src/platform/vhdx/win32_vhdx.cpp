#include "platform/vhdx/win32_vhdx.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <initguid.h>
#include <virtdisk.h>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "platform/git/git_cli.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

Error disk_error(DWORD code, std::string_view action, const fs::path& file) {
  Errc errc = Errc::io_error;
  switch (code) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      errc = Errc::in_use;
      break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      errc = Errc::not_found;
      break;
    case ERROR_ACCESS_DENIED:
      errc = Errc::access_denied;
      break;
    default:
      break;
  }
  return Error{.code = errc,
               .message = std::string(action) + " " + utf8_text(file) + ": Win32 error " +
                          std::to_string(code)};
}

// The value of environment variable `name`; empty when it is not set.
std::wstring variable(const wchar_t* name) {
  std::array<wchar_t, MAX_PATH> value{};
  const DWORD length =
      ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
  return length == 0 || length >= value.size() ? std::wstring{}
                                               : std::wstring(value.data(), length);
}

bool is_file(const fs::path& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

Result<void> Win32VirtualDisks::compact(const fs::path& file) {
  VIRTUAL_STORAGE_TYPE type{.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                            .VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
  OPEN_VIRTUAL_DISK_PARAMETERS open{};
  open.Version = OPEN_VIRTUAL_DISK_VERSION_2;
  wil::unique_handle disk;
  if (const DWORD opened = ::OpenVirtualDisk(&type, file.c_str(), VIRTUAL_DISK_ACCESS_NONE,
                                             OPEN_VIRTUAL_DISK_FLAG_NONE, &open, disk.put());
      opened != ERROR_SUCCESS) {
    return tl::unexpected(disk_error(opened, "cannot open the virtual disk", file));
  }
  COMPACT_VIRTUAL_DISK_PARAMETERS parameters{};
  parameters.Version = COMPACT_VIRTUAL_DISK_VERSION_1;
  if (const DWORD compacted =
          ::CompactVirtualDisk(disk.get(), COMPACT_VIRTUAL_DISK_FLAG_NONE, &parameters, nullptr);
      compacted != ERROR_SUCCESS) {
    return tl::unexpected(disk_error(compacted, "cannot compact", file));
  }
  return {};
}

std::optional<fs::path> wsl_path() {
  std::vector<fs::path> candidates;
  if (const std::wstring programs = variable(L"ProgramW6432"); !programs.empty()) {
    candidates.push_back(fs::path(programs) / L"WSL" / L"wsl.exe");
  }
  if (const std::wstring windows = variable(L"SystemRoot"); !windows.empty()) {
    candidates.push_back(fs::path(windows) / L"System32" / L"wsl.exe");
  }
  for (const fs::path& candidate : candidates) {
    if (is_file(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

Result<void> Win32Wsl::shutdown() {
  const std::optional<fs::path> program = wsl_path();
  if (!program) {
    return fail(Errc::not_found, "WSL is not installed: no wsl.exe");
  }
  std::optional<ProcessIdentity> identity;
  if (identity_) {
    Result<ProcessIdentity> user = identity_();
    if (!user) {
      return tl::unexpected(user.error());
    }
    identity = *std::move(user);
  }
  const std::vector<std::wstring> arguments = {L"--shutdown"};
  const Result<ProcessOutput> output =
      run_hidden(*program, arguments, timeout_, identity ? &*identity : nullptr);
  if (!output) {
    return tl::unexpected(output.error());
  }
  if (output->exit_code != 0) {
    return fail(Errc::io_error, "wsl --shutdown exited with " + std::to_string(output->exit_code));
  }
  return {};
}

}  // namespace myclean::platform
