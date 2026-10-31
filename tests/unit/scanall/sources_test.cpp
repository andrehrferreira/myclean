#include "core/scanall/sources.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/engine.hpp"
#include "core/clean/probes.hpp"
#include "core/compress/analyzer.hpp"
#include "core/compress/compression.hpp"
#include "core/drives/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/rules/schema.hpp"
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

class NoEnvironment final : public clean::IEnvironment {
 public:
  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view /*name*/) const override {
    return std::nullopt;
  }
};

map::Area area(std::wstring path, map::ReclaimKind kind, std::string ref = {},
               map::Category category = map::Category::other) {
  return map::Area{.id = "area:" + fs::path(path).string(),
                   .paths = {std::move(path)},
                   .category = category,
                   .label = "Label",
                   .reclaim = map::Reclaim{.kind = kind, .ref = std::move(ref), .note = "note"}};
}

const Finding& at(const std::vector<Finding>& findings, const fs::path& path) {
  const auto found = std::ranges::find(findings, path, &Finding::path);
  if (found == findings.end()) {
    throw std::runtime_error("no finding at " + path.string());
  }
  return *found;
}

const Finding& value(const std::optional<Finding>& finding) {
  if (!finding) {
    throw std::runtime_error("no finding");
  }
  return *finding;
}

std::vector<DriveInfo> two_drives() {
  std::vector<DriveInfo> drives = {drive(LR"(C:\)", DriveClass::nvme),
                                   drive(LR"(D:\)", DriveClass::nvme)};
  drives[0].system = true;
  return drives;
}

TEST(MapFindings, EachReclaimKindGetsItsActionAndPlan) {
  FakeDirectoryReader reader;
  reader.listings[LR"(C:\)"] = {file("hiberfil.sys", 8_GiB, now, 1),
                                file("pagefile.sys", 10_GiB, now, 2),
                                folder("Temp"),
                                folder("WinSxS"),
                                folder("Gone"),
                                folder("Bin"),
                                folder("Apps"),
                                folder("Installer"),
                                folder("npm"),
                                folder("Tiny"),
                                folder("Unknown")};
  const std::vector<std::pair<std::string, Bytes>> folders = {
      {"Temp", 1_GiB},      {"WinSxS", 2_GiB}, {"Gone", 1_GiB}, {"Bin", 1_GiB},    {"Apps", 3_GiB},
      {"Installer", 1_GiB}, {"npm", 1_GiB},    {"Tiny", 1_MiB}, {"Unknown", 2_GiB}};
  std::uint8_t id = 10;
  for (const auto& [name, size] : folders) {
    reader.listings[fs::path(LR"(C:\)") / name] = {file("f", size, now, id++)};
  }
  const space::ScanResult tree = scan_ok(reader, LR"(C:\)");
  const NoEnvironment environment;
  Result<map::DiskMap> map =
      map::build_map(tree,
                     {area(LR"(C:\hiberfil.sys)", map::ReclaimKind::setting, "hibernation"),
                      area(LR"(C:\pagefile.sys)", map::ReclaimKind::setting, "page_file"),
                      area(LR"(C:\Temp)", map::ReclaimKind::clean_rule, "t.delete"),
                      area(LR"(C:\WinSxS)", map::ReclaimKind::clean_rule, "t.native"),
                      area(LR"(C:\Gone)", map::ReclaimKind::clean_rule, "t.missing"),
                      area(LR"(C:\Bin)", map::ReclaimKind::handler, "Recycle Bin"),
                      area(LR"(C:\Apps)", map::ReclaimKind::uninstall),
                      area(LR"(C:\Installer)", map::ReclaimKind::manual_review),
                      area(LR"(C:\npm)", map::ReclaimKind::relocate, "npm"),
                      area(LR"(C:\Tiny)", map::ReclaimKind::uninstall)},
                     environment, std::nullopt, {.min_size = 1_GiB});
  ASSERT_TRUE(map) << map.error().message;
  const std::vector<rules::CleaningRule> rules = {
      {.id = "t.delete", .strategy = rules::Strategy::remove},
      {.id = "t.native", .strategy = rules::Strategy::native, .command = "dism /cleanup"}};
  const std::vector<DriveInfo> drives = two_drives();

  const std::vector<Finding> findings = map_findings(*map, drives[0], rules, drives, 64_MiB);

  ASSERT_EQ(findings.size(), 8U);
  const Finding& hibernation = at(findings, LR"(C:\hiberfil.sys)");
  EXPECT_EQ(hibernation.group, ActionGroup::remove);
  EXPECT_EQ(hibernation.bytes, 4_GiB);
  EXPECT_EQ(*hibernation.plan.param("mode"), "reduced");
  const Finding& page_file = at(findings, LR"(C:\pagefile.sys)");
  EXPECT_EQ(page_file.group, ActionGroup::relocate);
  EXPECT_EQ(page_file.bytes, 6_GiB);
  EXPECT_EQ(*page_file.plan.param("target"), R"(D:\)");
  const Finding& native = at(findings, LR"(C:\WinSxS)");
  EXPECT_EQ(native.group, ActionGroup::remove);
  EXPECT_EQ(native.plan.kind, PlanKind::native_command);
  EXPECT_EQ(*native.plan.param("rule"), "t.native");
  EXPECT_EQ(at(findings, LR"(C:\Gone)").group, ActionGroup::manual_review);
  EXPECT_EQ(at(findings, LR"(C:\Bin)").plan.kind, PlanKind::cleanup_handler);
  EXPECT_EQ(at(findings, LR"(C:\Apps)").group, ActionGroup::uninstall_review);
  EXPECT_EQ(at(findings, LR"(C:\Installer)").group, ActionGroup::manual_review);
  const Finding& unknown = at(findings, LR"(C:\Unknown)");
  EXPECT_EQ(unknown.group, ActionGroup::manual_review);
  EXPECT_EQ(unknown.reason, "no attribution explains it");
}

TEST(RuleFindings, CandidatesBecomeRemoveFindingsWithTheirRulePlan) {
  const std::vector<DriveInfo> drives = two_drives();
  const clean::Evaluation evaluation{
      .candidates = {{.rule_id = "r.del",
                      .tier = Tier::automatic,
                      .strategy = rules::Strategy::remove,
                      .path = LR"(C:\Temp\old)",
                      .size = 5_MiB},
                     {.rule_id = "r.report",
                      .tier = Tier::report,
                      .strategy = rules::Strategy::quarantine,
                      .path = LR"(D:\x)",
                      .size = 1_MiB},
                     {.rule_id = "t.native", .strategy = rules::Strategy::native, .size = 1_GiB},
                     {.rule_id = "r.handler",
                      .strategy = rules::Strategy::handler,
                      .size = 2_MiB,
                      .handler = "Thumbnail Cache"},
                     {.rule_id = "r.elsewhere", .path = LR"(Z:\foo)", .size = 1_MiB},
                     {.rule_id = "r.empty", .path = LR"(C:\empty)"}}};
  const std::vector<Finding> earlier = {
      Finding{.plan = Plan{.kind = PlanKind::native_command, .params = {{"rule", "t.native"}}}}};

  const std::vector<Finding> findings = rule_findings(evaluation, "rules", earlier, drives);

  ASSERT_EQ(findings.size(), 3U);
  EXPECT_EQ(findings[0].group, ActionGroup::remove);
  EXPECT_EQ(findings[0].plan.kind, PlanKind::delete_items);
  EXPECT_DOUBLE_EQ(findings[0].confidence, 0.95);
  ASSERT_EQ(findings[0].items.size(), 1U);
  EXPECT_EQ(findings[1].group, ActionGroup::manual_review);
  EXPECT_EQ(findings[1].plan.kind, PlanKind::quarantine_items);
  EXPECT_EQ(findings[1].drive, fs::path(LR"(D:\)"));
  EXPECT_EQ(findings[2].plan.kind, PlanKind::cleanup_handler);
  EXPECT_EQ(findings[2].drive, fs::path(LR"(C:\)"));
  EXPECT_TRUE(findings[2].items.empty());
}

TEST(CompressFinding, TheRecommendedAlgorithmsSavings) {
  const std::vector<DriveInfo> drives = two_drives();
  compress::FolderEstimate estimate{
      .folder = LR"(C:\Program Files\Tool)",
      .category = compress::Category::program,
      .size_on_disk = 10_GiB,
      .eligible = 8_GiB,
      .eligible_files = 100,
      .estimates = {{.algorithm = compress::Algorithm::lzx, .savings = 3_GiB, .confidence = 0.8}},
      .recommended = compress::Algorithm::lzx,
      .reason = "a program"};

  const std::optional<Finding> finding = compress_finding(estimate, drives[0]);

  ASSERT_TRUE(finding);
  EXPECT_EQ(value(finding).group, ActionGroup::compress);
  EXPECT_EQ(value(finding).bytes, 3_GiB);
  EXPECT_EQ(footprint(value(finding)), 10_GiB);
  EXPECT_DOUBLE_EQ(value(finding).confidence, 0.8);
  EXPECT_EQ(*value(finding).plan.param("algorithm"), "lzx");
  estimate.recommended.reset();
  EXPECT_FALSE(compress_finding(estimate, drives[0]));
}

TEST(MaintenanceFinding, RecommendedHeldOrNothing) {
  drives::VolumeAnalysis analysis{.drive = {.root = LR"(D:\)"},
                                  .needed = drives::Maintenance::retrim,
                                  .recommended = drives::Maintenance::retrim,
                                  .reasons = {"the last retrim is 40 days old"}};

  std::optional<Finding> finding = maintenance_finding(analysis);
  ASSERT_TRUE(finding);
  EXPECT_EQ(value(finding).group, ActionGroup::defrag_trim);
  EXPECT_EQ(value(finding).plan.kind, PlanKind::retrim_volume);
  EXPECT_EQ(*value(finding).plan.param("volume"), R"(D:\)");
  EXPECT_EQ(value(finding).bytes, 0_B);

  analysis.needed = drives::Maintenance::defrag;
  analysis.recommended = drives::Maintenance::none;
  finding = maintenance_finding(analysis);
  ASSERT_TRUE(finding);
  EXPECT_EQ(value(finding).group, ActionGroup::manual_review);

  analysis.needed = drives::Maintenance::none;
  EXPECT_FALSE(maintenance_finding(analysis));
}

}  // namespace
}  // namespace myclean::scanall
