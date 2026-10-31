#include "platform/settings/seek_penalty.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <wil/resource.h>
#include <windows.h>
#include <winioctl.h>

namespace myclean::platform {

std::optional<bool> Win32SeekPenalty::seek_penalty(std::uint32_t disk_number) const {
  const std::wstring device = LR"(\\.\PhysicalDrive)" + std::to_wstring(disk_number);
  const wil::unique_hfile disk{::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr, OPEN_EXISTING, 0, nullptr)};
  if (!disk) {
    return std::nullopt;
  }
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
  DWORD returned = 0;
  if (::DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &descriptor, sizeof(descriptor), &returned, nullptr) == FALSE ||
      returned < sizeof(descriptor)) {
    return std::nullopt;
  }
  return descriptor.IncursSeekPenalty != FALSE;
}

}  // namespace myclean::platform
