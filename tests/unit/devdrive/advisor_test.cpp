#include "core/devdrive/advisor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "clean/clean_fakes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/map/attribution.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"

namespace myclean::devdrive {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace myclean::literals;

// 2026-11-10 12:00:00 UTC.
const WallTime now = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;

bool any_step_has(const Advice& advice, std::string_view text) {
  return std::ranges::any_of(
      advice.steps, [&](const std::string& step) { return step.find(text) != std::string::npos; });
}

placement::TargetVolume volume(const wchar_t* root, std::uint64_t size_gb, std::uint64_t free_gb,
                               placement::DriveClass drive_class, bool healthy = true) {
  return placement::TargetVolume{.root = root,
                                 .size = Bytes{size_gb * 1'000'000'000ULL},
                                 .free = Bytes{free_gb * 1'000'000'000ULL},
                                 .drive_class = drive_class,
                                 .healthy = healthy};
}

TEST(DevDrive, IsSupportedFromBuild22621Revision2338) {
  EXPECT_FALSE(supported(Support{.build = 19045, .revision = 5000}));
  EXPECT_FALSE(supported(Support{.build = 22621, .revision = 2337}));
  EXPECT_TRUE(supported(Support{.build = 22621, .revision = 2338}));
  EXPECT_TRUE(supported(Support{.build = 22631, .revision = 1}));
  EXPECT_TRUE(supported(Support{.build = 26200, .revision = 0}));
}

TEST(DevDrive, FindsActiveRepositoriesOutermostOnly) {
  clean::testing::FakeTree tree;
  tree.add_directory(L"F:\\app\\.git", now - 2h);
  tree.add_file(L"F:\\app\\src\\main.cpp", 3_MiB, now - 1h);
  tree.add_directory(L"F:\\app\\vendor\\lib\\.git", now - 400h);  // inside app: moves with it
  tree.add_directory(L"F:\\old\\.git", now - std::chrono::days{90});
  tree.add_file(L"F:\\old\\readme.md", 1_KiB, now - std::chrono::days{90});
  tree.add_directory(L"F:\\web\\node_modules\\pkg\\.git", now - 1h);  // a skipped name
  tree.add_directory(L"F:\\deep\\a\\b\\c\\.git", now - 1h);           // deeper than max_depth
  tree.add_directory(L"W:\\game\\.git", now - 5h);
  tree.add_file(L"W:\\game\\big.uasset", 2_GiB, now - 5h);
  const clean::Workspaces workspaces{
      .roots = {L"F:\\", L"W:\\"}, .skip = {L"node_modules", L".git"}, .max_depth = 3};

  const std::vector<Repository> found = active_repositories(tree, workspaces, now);

  ASSERT_EQ(found.size(), 2U);
  EXPECT_EQ(found[0].path, fs::path{L"W:\\game"});  // largest first
  EXPECT_EQ(found[0].size, 2_GiB);
  EXPECT_EQ(found[0].changed, now - 5h);
  EXPECT_EQ(found[1].path, fs::path{L"F:\\app"});
  EXPECT_EQ(found[1].size, 3_MiB);
}

TEST(DevDrive, ListsTheDevCachesOfThePacksThatExistAndHoldSomething) {
  clean::testing::FakeTree tree;
  clean::testing::FakeEnvironment environment;
  environment.set(L"LOCALAPPDATA", L"C:\\Users\\owner\\AppData\\Local");
  tree.add_file(L"C:\\Users\\owner\\AppData\\Local\\npm-cache\\_cacache\\x", 4_GiB, now);
  tree.add_file(L"C:\\Users\\owner\\AppData\\Local\\pnpm\\store\\y", 1_GiB, now);
  tree.add_directory(L"C:\\Users\\owner\\AppData\\Local\\Yarn", now);  // empty
  tree.add_file(L"C:\\Users\\owner\\AppData\\Local\\Chrome\\cache", 9_GiB, now);
  const std::vector<map::Area> areas = {
      {.id = "node/npm-cache",
       .paths = {L"%LOCALAPPDATA%\\npm-cache"},
       .category = map::Category::dev_caches,
       .label = "npm cache",
       .reclaim = {.kind = map::ReclaimKind::relocate, .ref = "npm", .note = ""}},
      {.id = "node/pnpm-store",
       .paths = {L"%LOCALAPPDATA%\\pnpm"},
       .category = map::Category::dev_caches,
       .label = "pnpm content store",
       .reclaim = {.kind = map::ReclaimKind::clean_rule, .ref = "pnpm-store", .note = "trim"}},
      {.id = "node/yarn-cache",
       .paths = {L"%LOCALAPPDATA%\\Yarn"},
       .category = map::Category::dev_caches,
       .label = "Yarn cache",
       .reclaim = {}},
      {.id = "browsers/chrome",
       .paths = {L"%LOCALAPPDATA%\\Chrome"},
       .category = map::Category::browser_caches,
       .label = "Chrome",
       .reclaim = {}},
      {.id = "go/cache",
       .paths = {L"%GOCACHE%"},  // not set here
       .category = map::Category::dev_caches,
       .label = "Go build cache",
       .reclaim = {}}};

  const std::vector<PackageCache> caches = package_caches(tree, areas, environment);

  ASSERT_EQ(caches.size(), 2U);
  EXPECT_EQ(caches[0].area, "node/npm-cache");
  EXPECT_EQ(caches[0].size, 4_GiB);
  EXPECT_EQ(caches[0].relocation, "npm");
  EXPECT_EQ(caches[1].area, "node/pnpm-store");
  EXPECT_EQ(caches[1].relocation, "");
  EXPECT_EQ(caches[1].note, "trim");
}

Inputs owner_inputs() {
  return Inputs{
      .support = {.build = 26200, .revision = 7171},
      .volumes = {{.root = L"C:\\", .file_system = "NTFS"},
                  {.root = L"D:\\", .file_system = "NTFS"},
                  {.root = L"F:\\", .file_system = "NTFS"}},
      .targets = {volume(L"C:\\", 1000, 300, placement::DriveClass::nvme),
                  volume(L"D:\\", 4000, 1400, placement::DriveClass::nvme),
                  volume(L"F:\\", 2000, 1500, placement::DriveClass::nvme, false),
                  volume(L"L:\\", 10000, 4000, placement::DriveClass::hdd)},
      .repositories = {{.path = L"F:\\MyClean", .size = Bytes{30'000'000'000ULL}, .changed = now}},
      .caches = {{.area = "node/npm-cache",
                  .label = "npm cache",
                  .path = L"C:\\Users\\owner\\AppData\\Local\\npm-cache",
                  .size = Bytes{8'000'000'000ULL},
                  .relocation = "npm",
                  .note = ""},
                 {.area = "vcpkg/archives",
                  .label = "vcpkg binary cache",
                  .path = L"C:\\Users\\owner\\AppData\\Local\\vcpkg\\archives",
                  .size = Bytes{2'000'000'000ULL},
                  .relocation = "",
                  .note = "VCPKG_DEFAULT_BINARY_CACHE moves it."}}};
}

TEST(DevDrive, WithoutADevDriveExplainsSizesAndGuidesCreatingOneOnAVirtualDisk) {
  const Advice advice = advise(owner_inputs());

  EXPECT_TRUE(advice.supported);
  EXPECT_TRUE(advice.dev_drives.empty());
  EXPECT_NE(advice.explanation.find("ReFS"), std::string::npos);
  EXPECT_NE(advice.explanation.find("performance mode"), std::string::npos);
  EXPECT_EQ(advice.repositories.size(), 1U);
  EXPECT_EQ(advice.caches.size(), 2U);
  EXPECT_EQ(advice.total, Bytes{40'000'000'000ULL});
  EXPECT_EQ(advice.recommended_size, Bytes{60'000'000'000ULL});  // 40 GB * 1.5
  ASSERT_TRUE(advice.host.has_value());
  EXPECT_EQ(advice.host.value_or(placement::TargetVolume{}).root,
            fs::path{L"D:\\"});  // F: has more room but is not healthy
  EXPECT_TRUE(any_step_has(advice, "Create Dev Drive"));
  EXPECT_TRUE(any_step_has(advice, "Create new VHD: location D:\\"));
  EXPECT_TRUE(any_step_has(advice, "fsutil devdrv query"));
  EXPECT_TRUE(any_step_has(advice, "npm cache (8.0 GB)"));
  EXPECT_TRUE(any_step_has(advice, "Point vcpkg binary cache"));
  EXPECT_TRUE(any_step_has(advice, "1 active repositories (30.0 GB)"));

  const nlohmann::json json = advice;
  EXPECT_EQ(json.at("supported"), true);
  EXPECT_EQ(json.at("build"), "26200.7171");
  EXPECT_EQ(json.at("host").at("volume"), "D:");
  EXPECT_EQ(json.at("recommended_bytes"), 60'000'000'000ULL);
  EXPECT_EQ(json.at("repositories").at(0).at("path"), "F:\\MyClean");
  EXPECT_EQ(json.at("caches").at(0).at("relocation"), "npm");
  EXPECT_EQ(json.at("sources").at(0), "https://learn.microsoft.com/windows/dev-drive/");
}

TEST(DevDrive, NeverRecommendsLessThanFiftyGigabytesOrAHostWithoutRoom) {
  Inputs inputs = owner_inputs();
  inputs.repositories.clear();
  inputs.caches.resize(1);
  inputs.caches[0].size = Bytes{1'000'000'000ULL};
  inputs.targets = {volume(L"C:\\", 1000, 120, placement::DriveClass::nvme),  // 7 % after
                    volume(L"W:\\", 4000, 2000, placement::DriveClass::nvme_qlc)};
  const Advice advice = advise(inputs);
  EXPECT_EQ(advice.recommended_size, minimum_size);
  EXPECT_FALSE(advice.host.has_value());
  EXPECT_TRUE(any_step_has(advice, "back up first"));
  EXPECT_EQ(nlohmann::json(advice).at("host"), nullptr);
}

TEST(DevDrive, WithAnUntrustedDevDriveSuggestsTrustAndMovesOnlyWhatIsElsewhere) {
  Inputs inputs = owner_inputs();
  inputs.volumes.push_back({.root = L"E:\\", .file_system = "ReFS", .dev_drive = true});
  inputs.repositories.push_back(
      {.path = L"E:\\src\\tool", .size = Bytes{5'000'000'000ULL}, .changed = now});
  const Advice advice = advise(inputs);

  ASSERT_EQ(advice.dev_drives.size(), 1U);
  EXPECT_EQ(advice.repositories.size(), 1U);  // E:\src\tool is on the Dev Drive already
  EXPECT_FALSE(advice.host.has_value());
  EXPECT_FALSE(any_step_has(advice, "Create Dev Drive"));
  EXPECT_TRUE(any_step_has(advice, "fsutil devdrv trust E:"));
  EXPECT_TRUE(any_step_has(advice, "onto E:"));
  EXPECT_EQ(nlohmann::json(advice).at("dev_drives").at(0).at("volume"), "E:");
}

TEST(DevDrive, AnOlderWindowsGetsNoStepsAndSaysWhy) {
  Inputs inputs = owner_inputs();
  inputs.support = {.build = 19045, .revision = 6456};
  const Advice advice = advise(inputs);
  EXPECT_FALSE(advice.supported);
  EXPECT_TRUE(advice.steps.empty());
  ASSERT_FALSE(advice.notes.empty());
  EXPECT_NE(advice.notes.back().find("19045.6456"), std::string::npos);
  EXPECT_EQ(advice.repositories.size(), 1U);  // the estimate is still given
}

}  // namespace
}  // namespace myclean::devdrive
