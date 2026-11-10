#include "platform/devdrive/win32_dev_drive.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "core/devdrive/advisor.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {
namespace {

TEST(Win32DevDriveProbe, ReadsTheWindowsBuild) {
  const Win32DevDriveProbe probe;
  const Result<devdrive::Support> support = probe.support();
  ASSERT_TRUE(support.has_value()) << support.error().message;
  EXPECT_GE(support->build, 10240U);  // Windows 10 or later
}

TEST(Win32DevDriveProbe, ListsTheSystemVolumeWithItsFileSystem) {
  const Win32DevDriveProbe probe;
  const Result<std::vector<devdrive::VolumeState>> volumes = probe.volumes();
  ASSERT_TRUE(volumes.has_value()) << volumes.error().message;
  const auto system = std::ranges::find(volumes.value(), std::filesystem::path(L"C:\\"),
                                        &devdrive::VolumeState::root);
  ASSERT_NE(system, volumes->end());
  EXPECT_FALSE(system->file_system.empty()) << system->note;
  if (system->file_system == "NTFS") {
    EXPECT_FALSE(system->dev_drive);  // only ReFS volumes are Dev Drives
  }
  EXPECT_TRUE(std::ranges::is_sorted(volumes.value(), {}, &devdrive::VolumeState::root));
}

}  // namespace
}  // namespace myclean::platform
