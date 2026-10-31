#include "core/placement/drives.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"
#include "placement_fakes.hpp"

namespace myclean::placement {
namespace {

using namespace literals;
using testing::FakeSeekPenalty;
using testing::owner_volumes;

health::DriveHealth drive(std::uint32_t disk, health::Bus bus, std::string model,
                          health::Status status) {
  health::DriveHealth result;
  result.reading.disk_number = disk;
  result.reading.bus = bus;
  result.reading.model = std::move(model);
  result.assessment.status = status;
  if (status != health::Status::good) {
    result.assessment.reasons = {"1 new media error"};
  }
  return result;
}

telemetry::VolumeStatus volume(std::string root, std::vector<std::uint32_t> disks) {
  return {.id = R"(\\?\Volume{1}\)",
          .mount_points = {std::move(root)},
          .size = 1000_GiB,
          .free = 500_GiB,
          .disks = std::move(disks)};
}

TEST(DriveClass, ComesFromBusSeekPenaltyAndKnownQlcModels) {
  EXPECT_EQ(classify(health::Bus::nvme, "WD_BLACK SN7100 4TB", false), DriveClass::nvme);
  EXPECT_EQ(classify(health::Bus::nvme, "CT4000P3SSD8", false), DriveClass::nvme_qlc);
  EXPECT_EQ(classify(health::Bus::ata, "Samsung SSD 870 QVO 4TB", false), DriveClass::sata_ssd);
  EXPECT_EQ(classify(health::Bus::ata, "WDC WD101EFBX-68B0AN0", true), DriveClass::hdd);
  EXPECT_EQ(classify(health::Bus::usb, "ROG ESD-S1C", false), DriveClass::usb);
  EXPECT_EQ(classify(health::Bus::other, "Virtual Disk", std::nullopt), DriveClass::unknown);
  EXPECT_TRUE(known_qlc_model("INTEL SSDPEKNW020T8"));
  EXPECT_TRUE(known_qlc_model("Sabrent Rocket Q 4TB"));
  EXPECT_FALSE(known_qlc_model("Samsung SSD 980 PRO 2TB"));
}

TEST(TargetVolumes, JoinVolumesWithTheHealthAndClassOfTheirDisks) {
  const std::vector<telemetry::VolumeStatus> volumes = {
      volume("D:\\", {4}), volume("W:\\", {5}), volume("G:\\", {3}),
      volume("K:\\", {0}), volume("X:\\", {9}), volume(R"(C:\mnt\data\)", {4})};
  const health::HealthView view{
      .drives = {drive(0, health::Bus::ata, "WDC WD101EFBX-68B0AN0", health::Status::good),
                 drive(3, health::Bus::nvme, "Samsung SSD 980 PRO 2TB", health::Status::warning),
                 drive(4, health::Bus::nvme, "WD_BLACK SN7100 4TB", health::Status::good),
                 drive(5, health::Bus::nvme, "CT4000P3SSD8", health::Status::good)}};
  FakeSeekPenalty seek;
  seek.penalties = {{0, true}, {3, false}, {4, false}, {5, false}};

  const std::vector<TargetVolume> targets = target_volumes(volumes, &view, seek);

  ASSERT_EQ(targets.size(), 5U);  // the folder mount point has no drive letter
  EXPECT_EQ(targets[0].root, L"D:\\");
  EXPECT_EQ(targets[0].drive_class, DriveClass::nvme);
  EXPECT_TRUE(targets[0].healthy);
  EXPECT_EQ(targets[1].drive_class, DriveClass::nvme_qlc);
  EXPECT_FALSE(targets[2].healthy);
  EXPECT_NE(targets[2].health_note.find("warning"), std::string::npos);
  EXPECT_NE(targets[2].health_note.find("media error"), std::string::npos);
  EXPECT_EQ(targets[3].drive_class, DriveClass::hdd);
  EXPECT_FALSE(targets[4].healthy);  // no reading for disk 9: unknown health
  EXPECT_EQ(targets[4].drive_class, DriveClass::unknown);
}

TEST(TargetVolumes, AreOfUnknownHealthBeforeTheFirstHealthRead) {
  const std::vector<telemetry::VolumeStatus> volumes = {volume("D:\\", {4})};
  const std::vector<TargetVolume> targets = target_volumes(volumes, nullptr, FakeSeekPenalty{});
  ASSERT_EQ(targets.size(), 1U);
  EXPECT_FALSE(targets[0].healthy);
}

TEST(BestTarget, PrefersAHealthyTlcNvmeOverMoreFreeSpaceOnQlcHddOrUsb) {
  const std::vector<TargetVolume> volumes = owner_volumes();
  const TargetVolume* best = best_target(volumes, R"(C:\Users\owner\.gradle)", 7_GiB);
  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->root, L"D:\\");  // not W: (QLC, more free) nor K:/L: (HDD, most free)
}

TEST(BestTarget, NeverOffersAnUnhealthyDriveOrTheSourceVolume) {
  std::vector<TargetVolume> volumes = owner_volumes();
  for (TargetVolume& volume : volumes) {
    if (volume.root != L"G:\\" && volume.root != L"C:\\") {
      volume.healthy = false;
      volume.health_note = "unknown";
    }
  }
  EXPECT_EQ(best_target(volumes, R"(C:\Temp)", 1_GiB)->root, L"G:\\");
  volumes[4].healthy = false;  // G: degrades
  EXPECT_EQ(best_target(volumes, R"(C:\Temp)", 1_GiB), nullptr);
}

TEST(BestTarget, FallsBackToQlcOnlyWhenNoBetterDriveHasRoom) {
  std::vector<TargetVolume> volumes = owner_volumes();
  EXPECT_EQ(best_target(volumes, R"(C:\x)", 1300_GiB)->root, L"W:\\");  // D: lacks room
  EXPECT_EQ(best_target(volumes, R"(C:\x)", 2000_GiB)->root, L"L:\\");  // HDD before USB
}

TEST(Unsuitable, KeepsTenPercentOfTheVolumeFree) {
  const TargetVolume volume{.root = L"F:\\",
                            .size = 1000_GiB,
                            .free = 150_GiB,
                            .drive_class = DriveClass::nvme,
                            .healthy = true};
  EXPECT_TRUE(unsuitable(volume, 50_GiB).empty());
  EXPECT_FALSE(unsuitable(volume, 60_GiB).empty());
}

TEST(Paths, CompareIgnoringCaseAndTrailingSeparators) {
  EXPECT_TRUE(same_path(R"(C:\Users\Owner\.cargo\)", R"(c:\users\owner\.CARGO)"));
  EXPECT_TRUE(inside(R"(D:\DevCaches\cargo)", R"(D:\DevCaches)"));
  EXPECT_FALSE(inside(R"(D:\DevCachesX)", R"(D:\DevCaches)"));
  EXPECT_TRUE(inside(R"(D:\a)", R"(D:\)"));
  EXPECT_EQ(drive_root(R"(d:\x)"), L"D:\\");
  EXPECT_EQ(volume_of(owner_volumes(), R"(w:\DevCaches)")->drive_class, DriveClass::nvme_qlc);
}

}  // namespace
}  // namespace myclean::placement
