#include "platform/disk/win32_volume_info.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::platform {
namespace {

using telemetry::VolumeStatus;

// "C:\" on most machines.
std::string system_drive() {
  std::array<wchar_t, MAX_PATH> windows{};
  const UINT length = ::GetSystemWindowsDirectoryW(windows.data(), MAX_PATH);
  EXPECT_GE(length, 3U);
  return {static_cast<char>(windows[0]), ':', '\\'};
}

TEST(Win32VolumeInfo, EveryMountedVolumeHasAPlausibleSizeAndTheSystemDriveIsMappedToADisk) {
  const Win32VolumeInfo info;
  const Result<std::vector<VolumeStatus>> volumes = info.volumes();
  ASSERT_TRUE(volumes.has_value()) << volumes.error().message;
  ASSERT_FALSE(volumes->empty());

  std::set<std::uint32_t> disks;
  for (const VolumeStatus& volume : *volumes) {
    EXPECT_TRUE(volume.id.starts_with(R"(\\?\Volume{)")) << volume.id;
    EXPECT_FALSE(volume.mount_points.empty());
    EXPECT_GT(volume.size, Bytes{0}) << volume.id;
    EXPECT_LE(volume.free, volume.size) << volume.id;
    disks.insert(volume.disks.begin(), volume.disks.end());
  }
  std::cout << volumes->size() << " mounted volumes on " << disks.size() << " physical disks\n";

  const auto system = std::ranges::find_if(*volumes, [](const VolumeStatus& volume) {
    return std::ranges::find(volume.mount_points, system_drive()) != volume.mount_points.end();
  });
  ASSERT_NE(system, volumes->end());
  EXPECT_GT(system->free, Bytes{0});
  EXPECT_FALSE(system->disks.empty());
}

}  // namespace
}  // namespace myclean::platform
