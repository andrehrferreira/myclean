#include "platform/devdrive/win32_dev_drive.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which it needs.
#include <winioctl.h>

#include "core/devdrive/advisor.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

constexpr const wchar_t* version_key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

std::string narrow(std::wstring_view text) {
  std::string result;
  for (const wchar_t c : text) {
    result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return result;
}

// The Dev Drive state of the volume whose root is `root` ("E:\"), or the reason it is unknown.
void read_dev_state(const std::wstring& root, devdrive::VolumeState& volume) {
  const wil::unique_hfile folder{::CreateFileW(
      root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  if (!folder) {
    volume.note =
        "its Dev Drive state cannot be read: Win32 error " + std::to_string(::GetLastError());
    return;
  }
  FILE_FS_PERSISTENT_VOLUME_INFORMATION query{};
  query.Version = 1;
  query.FlagMask = PERSISTENT_VOLUME_STATE_DEV_VOLUME | PERSISTENT_VOLUME_STATE_TRUSTED_VOLUME;
  FILE_FS_PERSISTENT_VOLUME_INFORMATION state{};
  DWORD returned = 0;
  if (::DeviceIoControl(folder.get(), FSCTL_QUERY_PERSISTENT_VOLUME_STATE, &query, sizeof(query),
                        &state, sizeof(state), &returned, nullptr) == FALSE) {
    volume.note =
        "its Dev Drive state cannot be read: Win32 error " + std::to_string(::GetLastError());
    return;
  }
  volume.dev_drive = (state.VolumeFlags & PERSISTENT_VOLUME_STATE_DEV_VOLUME) != 0;
  volume.trusted = (state.VolumeFlags & PERSISTENT_VOLUME_STATE_TRUSTED_VOLUME) != 0;
}

}  // namespace

Result<devdrive::Support> Win32DevDriveProbe::support() const {
  std::array<wchar_t, 32> build{};
  auto size = static_cast<DWORD>(build.size() * sizeof(wchar_t));
  if (const LSTATUS read = ::RegGetValueW(HKEY_LOCAL_MACHINE, version_key, L"CurrentBuildNumber",
                                          RRF_RT_REG_SZ, nullptr, build.data(), &size);
      read != ERROR_SUCCESS) {
    return fail(Errc::io_error,
                "cannot read the Windows build number: Win32 error " + std::to_string(read));
  }
  DWORD revision = 0;
  size = sizeof(revision);
  if (const LSTATUS read = ::RegGetValueW(HKEY_LOCAL_MACHINE, version_key, L"UBR", RRF_RT_REG_DWORD,
                                          nullptr, &revision, &size);
      read != ERROR_SUCCESS) {
    revision = 0;  // absent before Windows 10: no Dev Drive either way
  }
  devdrive::Support support{.revision = revision};
  for (const wchar_t c : std::wstring_view(build.data())) {
    if (c < L'0' || c > L'9') {
      break;
    }
    support.build = (support.build * 10) + static_cast<std::uint32_t>(c - L'0');
  }
  return support;
}

Result<std::vector<devdrive::VolumeState>> Win32DevDriveProbe::volumes() const {
  std::vector<devdrive::VolumeState> volumes;
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((drives & (1U << static_cast<unsigned>(letter - L'A'))) == 0) {
      continue;
    }
    const std::wstring root = std::wstring(1, letter) + L":\\";
    if (::GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
      continue;
    }
    devdrive::VolumeState volume{.root = std::filesystem::path(root)};
    std::array<wchar_t, MAX_PATH + 1> file_system{};
    if (::GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                file_system.data(),
                                static_cast<DWORD>(file_system.size())) == FALSE) {
      volume.note =
          "its file system cannot be read: Win32 error " + std::to_string(::GetLastError());
      volumes.push_back(std::move(volume));
      continue;
    }
    volume.file_system = narrow(std::wstring_view(file_system.data()));
    // Only ReFS volumes can be Dev Drives.
    if (volume.file_system == "ReFS") {
      read_dev_state(root, volume);
    }
    volumes.push_back(std::move(volume));
  }
  return volumes;
}

}  // namespace myclean::platform
