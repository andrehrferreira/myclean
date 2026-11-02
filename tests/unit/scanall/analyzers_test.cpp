#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/probes.hpp"
#include "core/health/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/placement.hpp"
#include "core/space/scanner.hpp"
#include "scanall/scanall_fakes.hpp"
#include "space/space_fakes.hpp"

namespace myclean::scanall {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;
using placement::DriveClass;
using space::testing::FakeDirectoryReader;
using space::testing::scan_ok;
using testing::drive;
using testing::file;
using testing::folder;
using testing::now;

const WallTime long_ago = std::chrono::sys_days{std::chrono::year{2025} / 1 / 10};
const WallTime recently = std::chrono::sys_days{std::chrono::year{2026} / 10 / 25};

class NoEnvironment final : public clean::IEnvironment {
 public:
  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view /*name*/) const override {
    return std::nullopt;
  }
};

map::DiskMap map_of(const space::ScanResult& tree, std::vector<map::Area> areas) {
  const NoEnvironment environment;
  Result<map::DiskMap> map =
      map::build_map(tree, std::move(areas), environment, std::nullopt, {.min_size = 1_B});
  if (!map) {
    throw std::runtime_error(map.error().message);
  }
  return std::move(*map);
}

map::Area area(std::wstring path, map::Category category, map::ReclaimKind kind,
               std::string ref = {}) {
  return map::Area{.id = "test.area",
                   .paths = {std::move(path)},
                   .category = category,
                   .label = "Label",
                   .reclaim = map::Reclaim{.kind = kind, .ref = std::move(ref)}};
}

// C:\Data holds an old folder (with an old subfolder), a recent one, one a process uses and a
// small old one; C:\Program Files holds an old program.
void fill_cold(FakeDirectoryReader& reader) {
  reader.listings[LR"(C:\)"] = {folder("Data"), folder("Program Files")};
  reader.listings[LR"(C:\Data)"] = {folder("InUse"), folder("New"), folder("Old"), folder("Small")};
  reader.listings[LR"(C:\Data\Old)"] = {file("a.bin", 2_GiB, long_ago, 1), folder("Sub")};
  reader.listings[LR"(C:\Data\Old\Sub)"] = {file("b.bin", 1_GiB, long_ago, 2)};
  reader.listings[LR"(C:\Data\New)"] = {file("c.bin", 2_GiB, recently, 3)};
  reader.listings[LR"(C:\Data\InUse)"] = {file("d.bin", 2_GiB, long_ago, 4)};
  reader.listings[LR"(C:\Data\Small)"] = {file("e.bin", 10_MiB, long_ago, 5)};
  reader.listings[LR"(C:\Program Files)"] = {folder("App")};
  reader.listings[LR"(C:\Program Files\App)"] = {file("f.bin", 3_GiB, long_ago, 6)};
}

TEST(FolderTimes, NewestChangeAndCompletenessPropagateUp) {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {folder("A"), folder("B")};
  space::ListedEntry copied = file("copy.bin", 1_MiB, long_ago, 1);
  copied.created = recently;  // copied recently: keeps its old write time
  reader.listings[LR"(C:\A)"] = {copied};
  reader.errors.emplace(LR"(C:\B)", Error{.code = Errc::access_denied, .message = "denied"});
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");

  const FolderTimes times = folder_times(tree);

  EXPECT_EQ(times.newest[0], recently);
  EXPECT_EQ(times.newest[1], recently);
  EXPECT_TRUE(times.complete[1]);
  EXPECT_FALSE(times.complete[2]);
  EXPECT_FALSE(times.complete[0]);
}

TEST(Cold, OutermostColdFolderOnNvmeIsArchivedToHealthyHdd) {
  FakeDirectoryReader reader;
  fill_cold(reader);
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const std::vector<DriveInfo> drives = {
      drive(LR"(C:\)", DriveClass::nvme), drive(LR"(D:\)", DriveClass::nvme),
      drive(LR"(K:\)", DriveClass::hdd),
      drive(LR"(L:\)", DriveClass::hdd, health::Status::warning, 900_GiB)};
  const std::vector<fs::path> in_use = {LR"(C:\Data\InUse\bin)"};

  const std::vector<Finding> cold =
      find_cold(tree, folder_times(tree), drives[0], drives, in_use, now);

  ASSERT_EQ(cold.size(), 1U);
  const Finding& finding = cold[0];
  EXPECT_EQ(finding.group, ActionGroup::archive);
  EXPECT_EQ(finding.path, fs::path(LR"(C:\Data\Old)"));
  EXPECT_EQ(finding.bytes, 3_GiB);
  EXPECT_EQ(finding.plan.kind, PlanKind::move_folder);
  ASSERT_NE(finding.plan.param("target"), nullptr);
  EXPECT_EQ(*finding.plan.param("target"), R"(K:\Archive\C\Data\Old)");
  EXPECT_NE(finding.reason.find("2025-01-10"), std::string::npos);
}

TEST(Cold, WithoutRoomOnAHealthyHddItIsOnlyReviewed) {
  FakeDirectoryReader reader;
  fill_cold(reader);
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const std::vector<DriveInfo> drives = {
      drive(LR"(C:\)", DriveClass::sata_ssd),
      drive(LR"(K:\)", DriveClass::hdd, health::Status::good, 50_GiB)};

  const std::vector<Finding> cold = find_cold(tree, folder_times(tree), drives[0], drives, {}, now);

  ASSERT_EQ(cold.size(), 2U);  // Old, and InUse now that no process uses it
  EXPECT_EQ(cold[0].group, ActionGroup::manual_review);
  EXPECT_EQ(cold[0].plan.kind, PlanKind::review);
}

TEST(Cold, HddAndUnreadableSubtreesAreNotJudged) {
  FakeDirectoryReader reader;
  fill_cold(reader);
  reader.errors.emplace(LR"(C:\Data\Old\Sub)",
                        Error{.code = Errc::access_denied, .message = "denied"});
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const std::vector<DriveInfo> drives = {drive(LR"(C:\)", DriveClass::nvme),
                                         drive(LR"(K:\)", DriveClass::hdd)};
  const std::vector<fs::path> in_use = {LR"(C:\Data\InUse)"};

  EXPECT_TRUE(find_cold(tree, folder_times(tree), drives[0], drives, in_use, now).empty());
  EXPECT_TRUE(find_cold(tree, folder_times(tree), drives[1], drives, in_use, now).empty());
}

TEST(Cold, BackupsNextToOtherContentAreProposedApartFromIt) {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {folder("Old"), folder("ToS")};
  // Only backups: proposed whole.
  reader.listings[LR"(C:\Old)"] = {folder("Backup 2023"), folder("Backup 2024")};
  reader.listings[LR"(C:\Old\Backup 2023)"] = {file("a.bin", 2_GiB, long_ago, 1)};
  reader.listings[LR"(C:\Old\Backup 2024)"] = {file("b.bin", 2_GiB, long_ago, 2)};
  // A store of archives next to a project tree: each is proposed on its own.
  reader.listings[LR"(C:\ToS)"] = {folder("Client"), folder("Packs")};
  reader.listings[LR"(C:\ToS\Client)"] = {file("c.uasset", 3_GiB, long_ago, 3)};
  reader.listings[LR"(C:\ToS\Packs)"] = {file("d.zip", 2_GiB, long_ago, 4),
                                         file("readme.txt", 1_MiB, long_ago, 5)};
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const std::vector<DriveInfo> drives = {drive(LR"(C:\)", DriveClass::nvme),
                                         drive(LR"(K:\)", DriveClass::hdd)};

  const std::vector<Finding> cold = find_cold(tree, folder_times(tree), drives[0], drives, {}, now);

  ASSERT_EQ(cold.size(), 3U);
  EXPECT_EQ(cold[0].path, fs::path(LR"(C:\Old)"));
  EXPECT_EQ(cold[1].path, fs::path(LR"(C:\ToS\Client)"));
  EXPECT_EQ(cold[2].path, fs::path(LR"(C:\ToS\Packs)"));
}

TEST(Placement, FailingDriveMovesItsDataOffFirst) {
  FakeDirectoryReader reader;
  reader.listings[LR"(K:\)"] = {folder("Media"), folder("Windows"), folder("tiny")};
  reader.listings[LR"(K:\Media)"] = {file("movie.mkv", 2_GiB, long_ago, 1), folder(".git")};
  reader.listings[LR"(K:\Media\.git)"] = {};
  reader.listings[LR"(K:\Windows)"] = {file("big.bin", 2_GiB, long_ago, 2)};
  reader.listings[LR"(K:\tiny)"] = {file("x", 1_MiB, long_ago, 3)};
  const space::ScanResult tree = scan_ok(reader, LR"(K:\)");
  const std::vector<DriveInfo> drives = {drive(LR"(K:\)", DriveClass::hdd, health::Status::warning),
                                         drive(LR"(D:\)", DriveClass::nvme),
                                         drive(LR"(U:\)", DriveClass::usb)};

  const std::vector<Finding> advice =
      advise_placement(tree, folder_times(tree), nullptr, drives[0], drives, now);

  ASSERT_EQ(advice.size(), 1U);
  EXPECT_EQ(advice[0].group, ActionGroup::relocate);
  EXPECT_TRUE(advice[0].urgent);
  EXPECT_EQ(advice[0].path, fs::path(LR"(K:\Media)"));
  EXPECT_EQ(*advice[0].plan.param("target"), R"(D:\Media)");
  EXPECT_NE(advice[0].reason.find("warning"), std::string::npos);
}

TEST(Placement, ActiveProjectOnQlcMovesToNvme) {
  FakeDirectoryReader reader;
  reader.listings[LR"(Q:\)"] = {folder("src")};
  reader.listings[LR"(Q:\src)"] = {folder("app"), folder("old")};
  reader.listings[LR"(Q:\src\app)"] = {folder(".git"), file("main.cpp", 100_MiB, recently, 1),
                                       folder("lib")};
  reader.listings[LR"(Q:\src\app\.git)"] = {};
  reader.listings[LR"(Q:\src\app\lib)"] = {folder(".git"), file("lib.cpp", 80_MiB, recently, 2)};
  reader.listings[LR"(Q:\src\app\lib\.git)"] = {};
  reader.listings[LR"(Q:\src\old)"] = {folder(".git"), file("old.cpp", 100_MiB, long_ago, 3)};
  reader.listings[LR"(Q:\src\old\.git)"] = {};
  const space::ScanResult tree = scan_ok(reader, LR"(Q:\)");
  std::vector<DriveInfo> drives = {drive(LR"(Q:\)", DriveClass::nvme_qlc),
                                   drive(LR"(K:\)", DriveClass::hdd, health::Status::good, 900_GiB),
                                   drive(LR"(D:\)", DriveClass::nvme)};

  const std::vector<Finding> advice =
      advise_placement(tree, folder_times(tree), nullptr, drives[0], drives, now);

  ASSERT_EQ(advice.size(), 1U);
  EXPECT_EQ(advice[0].path, fs::path(LR"(Q:\src\app)"));
  EXPECT_EQ(advice[0].bytes, 180_MiB);
  EXPECT_EQ(*advice[0].plan.param("target"), R"(D:\src\app)");

  drives[0].drive_class = DriveClass::nvme;
  EXPECT_TRUE(advise_placement(tree, folder_times(tree), nullptr, drives[0], drives, now).empty());
}

TEST(Placement, GamesOnUsbMoveToAnInternalSsd) {
  FakeDirectoryReader reader;
  reader.listings[LR"(U:\)"] = {folder("SteamLibrary")};
  reader.listings[LR"(U:\SteamLibrary)"] = {file("game.pak", 2_GiB, long_ago, 1)};
  const space::ScanResult tree = scan_ok(reader, LR"(U:\)");
  const map::DiskMap map = map_of(
      tree, {area(LR"(?:\SteamLibrary)", map::Category::games, map::ReclaimKind::uninstall)});
  const std::vector<DriveInfo> drives = {drive(LR"(U:\)", DriveClass::usb),
                                         drive(LR"(D:\)", DriveClass::sata_ssd)};

  const std::vector<Finding> advice =
      advise_placement(tree, folder_times(tree), &map, drives[0], drives, now);

  ASSERT_EQ(advice.size(), 1U);
  EXPECT_EQ(advice[0].group, ActionGroup::relocate);
  EXPECT_EQ(advice[0].path, fs::path(LR"(U:\SteamLibrary)"));
  EXPECT_EQ(*advice[0].plan.param("target"), R"(D:\SteamLibrary)");
}

TEST(Placement, CachesOnTheSystemDriveUseTheirRelocation) {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {folder("npm-cache")};
  reader.listings[LR"(C:\npm-cache)"] = {file("index", 500_MiB, recently, 1)};
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const map::DiskMap map = map_of(tree, {area(LR"(C:\npm-cache)", map::Category::dev_caches,
                                              map::ReclaimKind::relocate, "npm")});
  std::vector<DriveInfo> drives = {drive(LR"(C:\)", DriveClass::nvme),
                                   drive(LR"(D:\)", DriveClass::nvme)};
  drives[0].system = true;

  const std::vector<Finding> advice =
      advise_placement(tree, folder_times(tree), &map, drives[0], drives, now);

  ASSERT_EQ(advice.size(), 1U);
  EXPECT_EQ(advice[0].plan.kind, PlanKind::relocate_cache);
  EXPECT_EQ(*advice[0].plan.param("cache"), "npm");
  EXPECT_EQ(fs::path(*advice[0].plan.param("target")),
            placement::target_folder(placement::CacheKind::npm, LR"(D:\)"));

  drives[0].system = false;
  EXPECT_TRUE(advise_placement(tree, folder_times(tree), &map, drives[0], drives, now).empty());
}

}  // namespace
}  // namespace myclean::scanall
