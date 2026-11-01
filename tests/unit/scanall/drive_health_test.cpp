#include "core/scanall/drive_health.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/drives/analysis.hpp"
#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/placement.hpp"
#include "core/space/scanner.hpp"
#include "core/telemetry/volume_info.hpp"
#include "scanall/scanall_fakes.hpp"
#include "space/space_fakes.hpp"

namespace myclean::scanall {
namespace {

using Json = nlohmann::json;
using namespace myclean::literals;
using placement::DriveClass;
using space::testing::FakeDirectoryReader;
using space::testing::scan_ok;
using testing::file;
using testing::folder;
using testing::now;

const WallTime long_ago = std::chrono::sys_days{std::chrono::year{2025} / 7 / 10};

// Disks 0 and 1 rotate (the HDDs under K: and L:), the others do not.
class FakeSeekPenalty final : public placement::ISeekPenalty {
 public:
  [[nodiscard]] std::optional<bool> seek_penalty(std::uint32_t disk_number) const override {
    return std::set<std::uint32_t>{0, 1}.contains(disk_number);
  }
};

class NoTrim final : public drives::ITrimSupport {
 public:
  [[nodiscard]] std::optional<bool> trim_supported(std::uint32_t /*disk_number*/) const override {
    return std::nullopt;
  }
  [[nodiscard]] std::optional<bool> ntfs_trim_enabled() const override { return std::nullopt; }
};

// One drive of the service's disks.health reply (ipc/disk_health.hpp).
Json service_drive(int disk, const std::string& model, const std::string& bus,
                   const std::string& status, std::vector<std::string> reasons = {}) {
  const bool readable = status != "unknown";
  std::string source = readable ? "ata_smart" : "none";
  if (bus == "nvme") {
    source = "nvme_health_log";
  }
  return Json{{"key", "k" + std::to_string(disk)},
              {"disk", std::to_string(disk)},
              {"model", model},
              {"bus", bus},
              {"source", source},
              {"volumes", Json::array()},
              {"status", status},
              {"reasons", std::move(reasons)},
              {"readable", readable},
              {"unreadable_reason", readable ? Json(nullptr) : Json("SMART read: access denied")},
              {"temperature_c", nullptr}};
}

// The service's reply for this machine: K: on disk 0 and L: on disk 1 (9.1 TB HDDs), C: on the
// NVMe disk 2 and D: on the NVMe disk 3, with the given health of K:, L: and D:.
Json service_reply(const std::string& k, const std::string& l, const std::string& d = "good") {
  return Json{
      {"time", 1'793'400'000},
      {"drives",
       Json::array({service_drive(0, "WDC WD101EFBX-68B0AN0", "ata", k,
                                  k == "good" ? std::vector<std::string>{}
                                              : std::vector<std::string>{"1 pending sectors"}),
                    service_drive(1, "WDC WD101EFBX-68B0AN0", "ata", l),
                    service_drive(2, "Samsung SSD 970 PRO 1TB", "nvme", "good"),
                    service_drive(3, "Samsung SSD 980 PRO 2TB", "nvme", d)})}};
}

telemetry::VolumeStatus volume(const std::string& root, std::uint32_t disk, Bytes size,
                               Bytes free) {
  return telemetry::VolumeStatus{
      .id = "volume " + root, .mount_points = {root}, .size = size, .free = free, .disks = {disk}};
}

// The drives of this machine as scan-all sees them with the disk health `view`.
std::vector<DriveInfo> machine(const health::HealthView& view) {
  const std::vector<telemetry::VolumeStatus> volumes = {
      volume("C:\\", 2, 952_GiB, 94_GiB), volume("D:\\", 3, 3726_GiB, 990_GiB),
      volume("K:\\", 0, 9314_GiB, 5136_GiB), volume("L:\\", 1, 9314_GiB, 5947_GiB)};
  const FakeSeekPenalty seek;
  const NoTrim trim;
  return drive_infos(drives::drive_states(volumes, &view, seek, trim), view, LR"(C:\)");
}

// C:\Users\Public\Documents\ZBrushData2025 (18 GiB), unchanged since 2025-07, beside a file
// written this week.
space::ScanResult public_documents() {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {folder("Users")};
  reader.listings[LR"(C:\Users)"] = {folder("Public")};
  reader.listings[LR"(C:\Users\Public)"] = {folder("Documents")};
  reader.listings[LR"(C:\Users\Public\Documents)"] = {
      folder("ZBrushData2025"), file("notes.txt", 1_MiB, now - std::chrono::days{3}, 2)};
  reader.listings[LR"(C:\Users\Public\Documents\ZBrushData2025)"] = {
      file("brushes.zbp", 18_GiB, long_ago, 1)};
  return scan_ok(reader, LR"(C:\)");
}

std::vector<Finding> cold_on_c(const std::vector<DriveInfo>& drives) {
  const space::ScanResult tree = public_documents();
  return find_cold(tree, folder_times(tree), drives[0], drives, {}, now);
}

health::HealthView must_not_read_locally() {
  ADD_FAILURE() << "the local disk health was read although the service answered";
  return {};
}

TEST(DriveHealth, ServiceReplyBecomesTheHealthOfEachDisk) {
  const Result<health::HealthView> view =
      parse_service_health(service_reply("good", "failing", "warning"));

  ASSERT_TRUE(view.has_value()) << view.error().message;
  ASSERT_EQ(view->drives.size(), 4U);
  EXPECT_EQ(view->time, WallTime{std::chrono::seconds{1'793'400'000}});
  EXPECT_EQ(view->drives[0].reading.disk_number, 0U);
  EXPECT_EQ(view->drives[0].reading.bus, health::Bus::ata);
  EXPECT_EQ(view->drives[0].reading.source, health::Source::ata_smart);
  EXPECT_EQ(view->drives[0].assessment.status, health::Status::good);
  EXPECT_EQ(view->drives[1].assessment.status, health::Status::critical);
  EXPECT_EQ(view->drives[3].assessment.status, health::Status::warning);
  EXPECT_EQ(view->drives[3].reading.bus, health::Bus::nvme);
}

TEST(DriveHealth, MalformedServiceReplyIsRefused) {
  Json reply = service_reply("good", "good");
  reply["drives"][0]["status"] = "fine";
  EXPECT_FALSE(parse_service_health(reply).has_value());
  reply["drives"][0]["status"] = "good";
  reply["drives"][0]["disk"] = "zero";
  EXPECT_FALSE(parse_service_health(reply).has_value());
  EXPECT_FALSE(parse_service_health(Json{{"time", nullptr}}).has_value());
}

TEST(DriveHealth, HealthyHddFromTheServiceTakesTheColdFolder) {
  const ChosenHealth chosen =
      choose_health(Result<Json>(service_reply("good", "good")), &must_not_read_locally);
  const std::vector<DriveInfo> drives = machine(chosen.view);

  EXPECT_NE(chosen.note.find("from the MyClean service"), std::string::npos) << chosen.note;
  ASSERT_EQ(drives.size(), 4U);
  EXPECT_EQ(drives[2].drive_class, DriveClass::hdd);
  EXPECT_EQ(drives[2].health, health::Status::good);
  EXPECT_TRUE(drives[0].system);

  const std::vector<Finding> cold = cold_on_c(drives);
  ASSERT_EQ(cold.size(), 1U);
  EXPECT_EQ(cold[0].group, ActionGroup::archive);
  EXPECT_EQ(cold[0].plan.kind, PlanKind::move_folder);
  ASSERT_NE(cold[0].plan.param("target"), nullptr);
  // L: has the most free space of the healthy HDDs.
  EXPECT_EQ(*cold[0].plan.param("target"), R"(L:\Archive\C\Users\Public\Documents\ZBrushData2025)");
}

TEST(DriveHealth, DegradedOrUnknownDriveFromTheServiceIsNeverATarget) {
  const ChosenHealth chosen =
      choose_health(Result<Json>(service_reply("warning", "unknown")), &must_not_read_locally);
  const std::vector<DriveInfo> drives = machine(chosen.view);
  EXPECT_EQ(drives[2].health, health::Status::warning);
  EXPECT_EQ(drives[3].health, health::Status::unknown);

  const std::vector<Finding> cold = cold_on_c(drives);
  ASSERT_EQ(cold.size(), 1U);
  EXPECT_EQ(cold[0].group, ActionGroup::manual_review);
  EXPECT_EQ(cold[0].plan.kind, PlanKind::review);

  // Only L: is degraded now: K: takes the folder.
  const std::vector<Finding> to_k = cold_on_c(machine(
      choose_health(Result<Json>(service_reply("good", "failing")), &must_not_read_locally).view));
  ASSERT_EQ(to_k.size(), 1U);
  EXPECT_EQ(*to_k[0].plan.param("target"), R"(K:\Archive\C\Users\Public\Documents\ZBrushData2025)");
}

TEST(DriveHealth, FailingDriveFromTheServiceMovesItsDataToAHealthyHdd) {
  FakeDirectoryReader reader;
  reader.listings[LR"(D:\)"] = {folder("Media")};
  reader.listings[LR"(D:\Media)"] = {file("movie.mkv", 2_GiB, long_ago, 1)};
  const space::ScanResult tree = scan_ok(reader, LR"(D:\)");
  const std::vector<DriveInfo> drives =
      machine(choose_health(Result<Json>(service_reply("good", "failing", "warning")),
                            &must_not_read_locally)
                  .view);

  const std::vector<Finding> advice =
      advise_placement(tree, folder_times(tree), nullptr, drives[1], drives, now);

  ASSERT_EQ(advice.size(), 1U);
  EXPECT_EQ(advice[0].group, ActionGroup::relocate);
  EXPECT_TRUE(advice[0].urgent);
  EXPECT_EQ(advice[0].plan.kind, PlanKind::move_folder);
  // C: is an NVMe drive but would keep less than 10 % free; L: is failing.
  EXPECT_EQ(*advice[0].plan.param("target"), R"(K:\Media)");
}

TEST(DriveHealth, WithoutTheServiceTheLocalReadingIsUsedAndSaidSo) {
  // What a process that cannot read SMART sees: the HDDs are of unknown health.
  int local_reads = 0;
  const auto local = [&local_reads] {
    ++local_reads;
    Result<health::HealthView> view = parse_service_health(service_reply("unknown", "unknown"));
    return view ? *view : health::HealthView{};
  };

  const ChosenHealth chosen = choose_health(
      fail(Errc::io_error, "the service is not running: nothing listens on its pipe"), local);

  EXPECT_EQ(local_reads, 1);
  EXPECT_NE(chosen.note.find("read by this process, not the MyClean service (the service is not "
                             "running"),
            std::string::npos)
      << chosen.note;
  EXPECT_NE(chosen.note.find("disk 0 (WDC WD101EFBX-68B0AN0), disk 1 (WDC WD101EFBX-68B0AN0)"),
            std::string::npos)
      << chosen.note;
  const std::vector<Finding> cold = cold_on_c(machine(chosen.view));
  ASSERT_EQ(cold.size(), 1U);
  EXPECT_EQ(cold[0].group, ActionGroup::manual_review);
  EXPECT_NE(cold[0].reason.find("no healthy HDD keeps 10 % free after taking it"),
            std::string::npos);
}

TEST(DriveHealth, ServiceThatHasNotReadTheDisksYetIsNotUsed) {
  const auto local = [] {
    Result<health::HealthView> view = parse_service_health(service_reply("good", "good"));
    return view ? *view : health::HealthView{};
  };

  const ChosenHealth chosen =
      choose_health(Result<Json>(Json{{"time", nullptr}, {"drives", Json::array()}}), local);

  EXPECT_NE(chosen.note.find("it has not read the disks yet"), std::string::npos) << chosen.note;
  EXPECT_EQ(chosen.note.find("unknown health"), std::string::npos) << chosen.note;
  EXPECT_EQ(cold_on_c(machine(chosen.view))[0].group, ActionGroup::archive);
}

}  // namespace
}  // namespace myclean::scanall
