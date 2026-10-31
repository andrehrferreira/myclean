#include "core/user_tasks/drive_facts.hpp"

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::user_tasks {
namespace {

using tray_model::DriveHealthRow;
using tray_model::DriveStatus;
using tray_model::VolumeRow;

VolumeRow volume(const char* name, std::vector<std::string> disks) {
  return VolumeRow{.name = name,
                   .size = Bytes{1000},
                   .free = Bytes{600},
                   .system = false,
                   .fill = tray_model::FillState::not_filling,
                   .days_until_full = std::nullopt,
                   .disks = std::move(disks)};
}

DriveHealthRow drive(const char* disk, DriveStatus status) {
  DriveHealthRow row;
  row.disk = disk;
  row.status = status;
  return row;
}

// Placement spec, "Healthy Relocation Target", as the tray sees the drives.
TEST(DriveFactsTest, AVolumeIsHealthyOnlyWhenEveryDiskUnderItReadsGood) {
  const std::vector<VolumeRow> volumes = {
      volume("C:", {"0"}), volume("D:", {"2", "10"}),       volume("G:", {"3"}),
      volume("E:", {"7"}), volume(R"(D:\mnt\data)", {"2"}), volume("Z:", {})};
  const std::vector<DriveHealthRow> drives = {
      drive("0", DriveStatus::good), drive("2", DriveStatus::good), drive("10", DriveStatus::good),
      drive("3", DriveStatus::warning)};
  const std::vector<placement::TargetVolume> targets = target_volumes(volumes, drives);
  ASSERT_EQ(targets.size(), 5U) << "a folder mount has no drive letter";
  EXPECT_EQ(targets[0].root, LR"(C:\)");
  EXPECT_EQ(targets[0].size, Bytes{1000});
  EXPECT_EQ(targets[0].free, Bytes{600});
  EXPECT_TRUE(targets[0].healthy);
  EXPECT_TRUE(targets[1].healthy) << "both disks good";
  EXPECT_FALSE(targets[2].healthy);
  EXPECT_EQ(targets[2].health_note, "the health of disk 3 is warning");
  EXPECT_FALSE(targets[3].healthy) << "no reading of disk 7";
  EXPECT_EQ(targets[3].health_note, "the health of disk 7 is unknown");
  EXPECT_FALSE(targets[4].healthy);
  EXPECT_FALSE(placement::unsuitable(targets[2], Bytes{1}).empty());
}

TEST(DriveFactsTest, WithoutDiskHealthNoVolumeIsATarget) {
  const std::vector<placement::TargetVolume> targets =
      target_volumes({volume("D:", {"2"})}, std::nullopt);
  ASSERT_EQ(targets.size(), 1U);
  EXPECT_FALSE(targets[0].healthy);
}

}  // namespace
}  // namespace myclean::user_tasks
