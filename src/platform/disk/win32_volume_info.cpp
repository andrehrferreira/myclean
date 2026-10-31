#include "platform/disk/win32_volume_info.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winioctl.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::platform {

namespace {

using UniqueVolumeFind =
    wil::unique_any_handle_invalid<decltype(&::FindVolumeClose), ::FindVolumeClose>;

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

// The mount points of `volume` ("C:\", "D:\mnt\data\"), from a list of strings ending in "".
std::vector<std::string> mount_points(const std::wstring& volume) {
  std::vector<wchar_t> names(MAX_PATH);
  DWORD needed = 0;
  while (::GetVolumePathNamesForVolumeNameW(volume.c_str(), names.data(),
                                            static_cast<DWORD>(names.size()), &needed) == FALSE) {
    if (::GetLastError() != ERROR_MORE_DATA) {
      return {};
    }
    names.resize(needed);
  }
  std::vector<std::string> points;
  for (std::wstring_view rest{names.data(), names.size()};
       !rest.empty() && rest.front() != L'\0';) {
    const std::wstring_view name = rest.substr(0, rest.find(L'\0'));
    points.push_back(utf8(name));
    rest.remove_prefix(std::min(rest.size(), name.size() + 1));
  }
  return points;
}

// The physical disk numbers holding the extents of `volume`; empty when they cannot be read.
std::vector<std::uint32_t> disks_of(std::wstring volume) {
  if (volume.ends_with(L'\\')) {
    volume.pop_back();  // without the backslash the path names the volume device, not its root
  }
  const wil::unique_hfile device{::CreateFileW(
      volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (!device) {
    return {};
  }
  const std::size_t extents_offset = offsetof(VOLUME_DISK_EXTENTS, Extents);
  std::vector<std::byte> buffer(extents_offset + (4 * sizeof(DISK_EXTENT)));
  DWORD returned = 0;
  while (::DeviceIoControl(device.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                           buffer.data(), static_cast<DWORD>(buffer.size()), &returned,
                           nullptr) == FALSE) {
    if (::GetLastError() != ERROR_MORE_DATA) {
      return {};
    }
    DWORD count = 0;
    std::memcpy(&count, buffer.data(), sizeof(count));
    buffer.resize(extents_offset + (count * sizeof(DISK_EXTENT)));
  }
  DWORD count = 0;
  std::memcpy(&count, buffer.data(), sizeof(count));
  const std::span<const std::byte> extents = std::span<const std::byte>{buffer}.subspan(
      extents_offset,
      std::min<std::size_t>(count * sizeof(DISK_EXTENT), buffer.size() - extents_offset));
  std::vector<std::uint32_t> disks;
  for (std::size_t offset = 0; offset + sizeof(DISK_EXTENT) <= extents.size();
       offset += sizeof(DISK_EXTENT)) {
    DISK_EXTENT extent{};
    std::memcpy(&extent, extents.subspan(offset, sizeof(extent)).data(), sizeof(extent));
    if (std::ranges::find(disks, extent.DiskNumber) == disks.end()) {
      disks.push_back(extent.DiskNumber);
    }
  }
  std::ranges::sort(disks);
  return disks;
}

}  // namespace

Result<std::vector<telemetry::VolumeStatus>> Win32VolumeInfo::volumes() const {
  // A drive without media must fail quietly instead of asking the user to insert a disk.
  DWORD previous_mode = 0;
  ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
  const auto restore = wil::scope_exit([&] { ::SetThreadErrorMode(previous_mode, nullptr); });

  std::array<wchar_t, MAX_PATH + 1> name{};
  const UniqueVolumeFind find{::FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()))};
  if (!find) {
    return fail(Errc::io_error,
                "FindFirstVolumeW failed: Win32 error " + std::to_string(::GetLastError()));
  }
  std::vector<telemetry::VolumeStatus> volumes;
  for (bool more = true; more; more = ::FindNextVolumeW(find.get(), name.data(),
                                                        static_cast<DWORD>(name.size())) != FALSE) {
    const std::wstring volume{name.data()};
    std::vector<std::string> points = mount_points(volume);
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free{};
    if (points.empty() || ::GetDiskFreeSpaceExW(volume.c_str(), nullptr, &total, &free) == FALSE) {
      continue;
    }
    volumes.push_back(telemetry::VolumeStatus{.id = utf8(volume),
                                              .mount_points = std::move(points),
                                              .size = Bytes{total.QuadPart},
                                              .free = Bytes{free.QuadPart},
                                              .disks = disks_of(volume)});
  }
  if (const DWORD error = ::GetLastError(); error != ERROR_NO_MORE_FILES) {
    return fail(Errc::io_error, "FindNextVolumeW failed: Win32 error " + std::to_string(error));
  }
  return volumes;
}

}  // namespace myclean::platform
