#include "core/scanall/report.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/health/analysis.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
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

Finding finding(ActionGroup group, const fs::path& path, Bytes bytes, Bytes footprint,
                const fs::path& drive = LR"(C:\)") {
  return Finding{.group = group,
                 .source = "test",
                 .drive = drive,
                 .path = path,
                 .items = {Item{.path = path, .size = footprint}},
                 .bytes = bytes};
}

std::size_t index_of(ActionGroup group) {
  return static_cast<std::size_t>(group);
}

const Finding& at(const Report& report, ActionGroup group, const fs::path& path) {
  const auto found = std::ranges::find_if(report.findings, [&](const Finding& finding) {
    return finding.group == group && finding.path == path;
  });
  if (found == report.findings.end()) {
    throw std::runtime_error("no finding at " + path.string());
  }
  return *found;
}

// C:\Tools (90 GiB, with Cache of 10 GiB inside), C:\Media (20 GiB and a 1 GiB duplicate),
// C:\Old (5 GiB).
space::ScanResult machine() {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {folder("Media"), folder("Old"), folder("Tools")};
  reader.listings[LR"(C:\Tools)"] = {folder("Cache"), file("t.bin", 90_GiB, now, 1)};
  reader.listings[LR"(C:\Tools\Cache)"] = {file("c.bin", 10_GiB, now, 2)};
  reader.listings[LR"(C:\Media)"] = {file("dup.bin", 1_GiB, now, 3), file("m.bin", 20_GiB, now, 4)};
  reader.listings[LR"(C:\Old)"] = {file("o.bin", 5_GiB, now, 5)};
  return scan_ok(reader, LR"(C:\)");
}

TEST(Report, EachByteIsCountedUnderTheFirstActionThatCoversIt) {
  const std::vector<space::ScanResult> trees = {machine()};
  Finding no_items = finding(ActionGroup::remove, LR"(C:\Handler)", 3_GiB, 0_B);
  no_items.items.clear();
  std::vector<Finding> findings = {
      finding(ActionGroup::compress, LR"(C:\Tools)", 30_GiB, 100_GiB),
      finding(ActionGroup::remove, LR"(C:\Tools\Cache)", 10_GiB, 10_GiB),
      finding(ActionGroup::manual_review, LR"(C:\Tools\Cache)", 10_GiB, 10_GiB),
      finding(ActionGroup::uninstall_review, LR"(C:\Tools)", 90_GiB, 90_GiB),
      finding(ActionGroup::archive, LR"(C:\Media)", 21_GiB, 21_GiB),
      finding(ActionGroup::deduplicate, LR"(C:\Media\dup.bin)", 1_GiB, 1_GiB),
      finding(ActionGroup::remove, LR"(C:\Old)", 5_GiB, 5_GiB),
      finding(ActionGroup::remove, LR"(c:\old\)", 5_GiB, 5_GiB),
      no_items};

  const Report report =
      build_report({drive(LR"(C:\)", DriveClass::nvme)}, std::move(findings), trees, now);

  EXPECT_EQ(at(report, ActionGroup::remove, LR"(C:\Tools\Cache)").counted, 10_GiB);
  EXPECT_EQ(at(report, ActionGroup::compress, LR"(C:\Tools)").counted, 27_GiB);
  EXPECT_EQ(at(report, ActionGroup::manual_review, LR"(C:\Tools\Cache)").counted, 0_B);
  EXPECT_EQ(at(report, ActionGroup::uninstall_review, LR"(C:\Tools)").counted, 0_B);
  EXPECT_EQ(at(report, ActionGroup::deduplicate, LR"(C:\Media\dup.bin)").counted, 1_GiB);
  EXPECT_EQ(at(report, ActionGroup::archive, LR"(C:\Media)").counted, 20_GiB);
  EXPECT_EQ(at(report, ActionGroup::remove, LR"(C:\Old)").counted, 5_GiB);
  EXPECT_EQ(at(report, ActionGroup::remove, LR"(c:\old\)").counted, 0_B);
  EXPECT_EQ(at(report, ActionGroup::remove, LR"(C:\Handler)").counted, 3_GiB);

  EXPECT_EQ(report.totals.at(index_of(ActionGroup::remove)), 18_GiB);
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::deduplicate)), 1_GiB);
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::archive)), 20_GiB);
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::compress)), 27_GiB);
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::manual_review)), 0_B);
  EXPECT_EQ(report.drives.at(0).counted, report.totals);
}

TEST(Report, WithinOneActionTheMostPreciseFindingOwnsItsBytes) {
  const std::vector<space::ScanResult> trees = {machine()};
  std::vector<Finding> findings = {
      finding(ActionGroup::remove, LR"(C:\Tools)", 90_GiB, 90_GiB),  // without Cache, as a map
      finding(ActionGroup::remove, LR"(C:\Tools\Cache)", 10_GiB, 10_GiB)};

  const Report report =
      build_report({drive(LR"(C:\)", DriveClass::nvme)}, std::move(findings), trees, now);

  EXPECT_EQ(at(report, ActionGroup::remove, LR"(C:\Tools)").counted, 90_GiB);
  EXPECT_EQ(at(report, ActionGroup::remove, LR"(C:\Tools\Cache)").counted, 10_GiB);
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::remove)), 100_GiB);
}

TEST(Report, UnhealthyDriveHoldsWritesAndMovingOffComesFirst) {
  FakeDirectoryReader reader;
  reader.listings[LR"(K:\)"] = {folder("X"), folder("Y")};
  reader.listings[LR"(K:\X)"] = {file("x.bin", 1_GiB, now, 1)};
  reader.listings[LR"(K:\Y)"] = {file("y.bin", 5_GiB, now, 2)};
  const std::vector<space::ScanResult> trees = {scan_ok(reader, LR"(K:\)")};
  Finding urgent = finding(ActionGroup::relocate, LR"(K:\X)", 1_GiB, 1_GiB, LR"(K:\)");
  urgent.urgent = true;
  std::vector<Finding> findings = {
      finding(ActionGroup::remove, LR"(K:\X)", 1_GiB, 1_GiB, LR"(K:\)"),
      finding(ActionGroup::relocate, LR"(K:\Y)", 5_GiB, 5_GiB, LR"(K:\)"), urgent,
      finding(ActionGroup::remove, LR"(D:\Z)", 2_GiB, 2_GiB, LR"(D:\)")};

  const Report report = build_report({drive(LR"(K:\)", DriveClass::hdd, health::Status::warning),
                                      drive(LR"(D:\)", DriveClass::nvme)},
                                     std::move(findings), trees, now);

  const Finding& held = at(report, ActionGroup::manual_review, LR"(K:\X)");
  EXPECT_EQ(held.counted, 0_B);
  EXPECT_TRUE(held.reason.starts_with("held: the drive's health is warning"));
  EXPECT_EQ(report.findings[0].path, fs::path(LR"(D:\Z)"));
  EXPECT_EQ(report.findings[0].counted, 2_GiB);  // no tree: counted in full
  EXPECT_TRUE(report.findings[1].urgent);        // urgent first within relocate
  EXPECT_EQ(report.totals.at(index_of(ActionGroup::relocate)), 6_GiB);
  EXPECT_EQ(report.drives.at(1).counted.at(index_of(ActionGroup::remove)), 2_GiB);
}

TEST(Report, JsonCarriesDrivesTotalsAndPlans) {
  const std::vector<space::ScanResult> trees = {machine()};
  DriveInfo c = drive(LR"(C:\)", DriveClass::nvme);
  c.explained = 250_GiB;
  c.tree = "scanner";
  Finding remove = finding(ActionGroup::remove, LR"(C:\Old)", 5_GiB, 5_GiB);
  remove.plan = Plan{.kind = PlanKind::delete_items, .params = {{"rule", "r"}}};

  const nlohmann::json json = to_json(build_report({c}, {remove}, trees, now));

  EXPECT_EQ(json["time"], "2026-10-31T00:00:00Z");
  EXPECT_EQ(json["precedence"][0], "remove");
  EXPECT_EQ(json["totals"]["remove"], (5_GiB).count());
  EXPECT_EQ(json["drives"][0]["root"], R"(C:\)");
  EXPECT_EQ(json["drives"][0]["explained_percent"], 50.0);
  EXPECT_EQ(json["drives"][0]["counted"]["remove"], (5_GiB).count());
  EXPECT_EQ(json["findings"][0]["plan"]["kind"], "delete_items");
  EXPECT_EQ(json["findings"][0]["plan"]["params"]["rule"], "r");
  EXPECT_EQ(json["findings"][0]["items"][0]["size"], (5_GiB).count());
}

}  // namespace
}  // namespace myclean::scanall
