#include "core/scanall/owner_decisions.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clean/clean_fakes.hpp"
#include "core/health/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/scanall/findings.hpp"
#include "placement/placement_fakes.hpp"
#include "scanall/scanall_fakes.hpp"

namespace myclean::scanall {
namespace {

namespace fs = std::filesystem;
using placement::DriveClass;
using placement::RegistryType;
using placement::RegistryValue;
using testing::drive;

constexpr std::string_view machine = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::string_view machine32 =
    R"(HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::string_view user = R"(HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";

RegistryValue text(std::string value) {
  return RegistryValue{.text = std::move(value), .type = RegistryType::string};
}

RegistryValue number(std::uint64_t value) {
  return RegistryValue{.text = std::to_string(value), .type = RegistryType::dword};
}

// An Uninstall entry `name` under `root`\`subkey` installed at `location`.
void install(placement::testing::FakeRegistry& registry, std::string_view root,
             std::string_view subkey, const std::string& name, const std::string& location) {
  const std::string key = std::string(root) + "\\" + std::string(subkey);
  registry.put(key, "DisplayName", text(name));
  registry.put(key, "UninstallString", text("\"" + location + R"(\uninstall.exe" /x)"));
  registry.put(key, "InstallLocation", text(location));
}

map::Area area(const std::string& id, std::vector<std::wstring> paths, map::ReclaimKind kind,
               std::string note = {}) {
  return map::Area{.id = id,
                   .paths = std::move(paths),
                   .category = map::Category::apps,
                   .label = "Area " + id,
                   .reclaim = {.kind = kind, .ref = {}, .note = std::move(note)}};
}

TEST(OwnerDecisionsTest, TheArchiveFolderKeepsTheDriveLetterAndThePath) {
  EXPECT_EQ(archive_folder(LR"(C:\Users\owner\Old)", LR"(L:\)"),
            fs::path(LR"(L:\Archive\C\Users\owner\Old)"));
  EXPECT_EQ(archive_folder(LR"(d:\Projects)", LR"(K:\)"), fs::path(LR"(K:\Archive\D\Projects)"));
  EXPECT_TRUE(archive_folder(LR"(\\server\share\Old)", LR"(L:\)").empty());
}

TEST(OwnerDecisionsTest, TheArchiveGoesToTheHealthyHddWithTheMostRoom) {
  const Bytes size{std::uint64_t{100} << 30U};
  const std::vector<DriveInfo> drives = {
      drive(LR"(C:\)", DriveClass::nvme), drive(LR"(D:\)", DriveClass::nvme),
      drive(LR"(K:\)", DriveClass::hdd, health::Status::good, Bytes{std::uint64_t{300} << 30U}),
      drive(LR"(L:\)", DriveClass::hdd, health::Status::good, Bytes{std::uint64_t{600} << 30U}),
      drive(LR"(M:\)", DriveClass::hdd, health::Status::warning, Bytes{std::uint64_t{900} << 30U})};

  EXPECT_EQ(archive_target(drives, LR"(C:\Users\owner\Old)", size),
            std::optional<fs::path>(LR"(L:\Archive\C\Users\owner\Old)"));
  // No healthy HDD keeps 10 % free after taking it: an SSD with room is never chosen.
  EXPECT_EQ(archive_target(drives, LR"(C:\Users\owner\Old)", Bytes{std::uint64_t{550} << 30U}),
            std::nullopt);
}

TEST(OwnerDecisionsTest, DriveInfosTakeHealthFromThePlacementVolumes) {
  const std::vector<placement::TargetVolume> volumes = {
      placement::TargetVolume{.root = LR"(C:\)",
                              .size = Bytes{1000},
                              .free = Bytes{100},
                              .drive_class = DriveClass::nvme,
                              .healthy = true},
      placement::TargetVolume{.root = LR"(L:\)",
                              .size = Bytes{2000},
                              .free = Bytes{1500},
                              .drive_class = DriveClass::hdd,
                              .healthy = false,
                              .health_note = "pending sectors"}};

  const std::vector<DriveInfo> drives = drive_infos(volumes);

  ASSERT_EQ(drives.size(), 2U);
  EXPECT_EQ(drives[0].health, health::Status::good);
  EXPECT_EQ(drives[0].free, Bytes{100});
  EXPECT_EQ(drives[1].health, health::Status::unknown);
  EXPECT_EQ(drives[1].drive_class, DriveClass::hdd);
  EXPECT_EQ(drives[1].health_note, "pending sectors");
}

TEST(OwnerDecisionsTest, ReadsTheUninstallEntriesOfEveryRootThatCanBeUninstalled) {
  placement::testing::FakeRegistry registry;
  install(registry, machine, "{GUID-1}", "Tool One", R"("C:\Program Files\Tool One\")");
  registry.put(std::string(machine) + R"(\{GUID-1})", "Publisher", text("Vendor"));
  registry.put(std::string(machine) + R"(\{GUID-1})", "DisplayVersion", text("1.2"));
  registry.put(std::string(machine) + R"(\{GUID-1})", "EstimatedSize", number(2048));
  registry.put(std::string(machine) + R"(\{GUID-1})", "UninstallString",
               text("MsiExec.exe /X{GUID-1}"));
  install(registry, machine32, "Old", "Old Tool", R"(C:\Program Files (x86)\Old)");
  install(registry, user, "Mine", "My App", R"(C:\Users\owner\AppData\Local\Programs\Mine)");
  // Left out: a system component, an update of another entry, one without an uninstaller.
  install(registry, machine, "Runtime", "Hidden Runtime", R"(C:\Runtime)");
  registry.put(std::string(machine) + R"(\Runtime)", "SystemComponent", number(1));
  install(registry, machine, "Patch", "Tool One Update", R"(C:\Program Files\Tool One)");
  registry.put(std::string(machine) + R"(\Patch)", "ParentKeyName", text("{GUID-1}"));
  registry.put(std::string(machine) + R"(\Driver)", "DisplayName", text("A driver"));

  const Result<std::vector<UninstallEntry>> entries = uninstall_entries(registry);

  ASSERT_TRUE(entries.has_value()) << entries.error().message;
  ASSERT_EQ(entries->size(), 3U);
  const UninstallEntry& tool = entries->front();
  EXPECT_EQ(tool.key, std::string(machine) + R"(\{GUID-1})");
  EXPECT_EQ(tool.name, "Tool One");
  EXPECT_EQ(tool.publisher, "Vendor");
  EXPECT_EQ(tool.version, "1.2");
  // Quotes and the trailing separator are dropped from the location.
  EXPECT_EQ(tool.location, fs::path(LR"(C:\Program Files\Tool One)"));
  EXPECT_EQ(tool.uninstall, "MsiExec.exe /X{GUID-1}");
  EXPECT_EQ(tool.estimated, Bytes{std::uint64_t{2048} * 1024});
  EXPECT_EQ((*entries)[1].name, "Old Tool");
  EXPECT_EQ((*entries)[2].name, "My App");
  EXPECT_EQ((*entries)[2].estimated, Bytes{});
}

TEST(OwnerDecisionsTest, AnEntryWithoutANameOrUninstallerIsNotAProgramToUninstall) {
  placement::testing::FakeRegistry registry;
  const std::string key = std::string(machine) + R"(\Gone)";
  EXPECT_EQ(uninstall_entry(registry, key).value(), std::nullopt);
  registry.put(key, "DisplayName", text("Half"));
  EXPECT_EQ(uninstall_entry(registry, key).value(), std::nullopt);
  registry.put(key, "UninstallString", text("MsiExec.exe /X{GUID}"));
  const Result<std::optional<UninstallEntry>> entry = uninstall_entry(registry, key);
  ASSERT_TRUE(entry.has_value());
  const std::optional<UninstallEntry>& found = *entry;
  ASSERT_TRUE(found.has_value());
  const UninstallEntry program = found.value_or(UninstallEntry{.location = L"unset"});
  EXPECT_EQ(program.name, "Half");
  EXPECT_TRUE(program.location.empty());
}

// Apps spec, "Uninstall Through The App's Own Uninstaller": QuietUninstallString serves when
// there is no UninstallString.
TEST(OwnerDecisionsTest, AQuietUninstallerAloneMakesAnEntry) {
  placement::testing::FakeRegistry registry;
  const std::string key = std::string(machine) + R"(\Quiet)";
  registry.put(key, "DisplayName", text("Quiet"));
  const std::string quiet = R"("C:\Program Files\Q\unins.exe" /S)";
  registry.put(key, "QuietUninstallString", text(quiet));
  const Result<std::optional<UninstallEntry>> entry = uninstall_entry(registry, key);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry.value_or(std::nullopt).value_or(UninstallEntry{}).uninstall, quiet);
}

TEST(OwnerDecisionsTest, ProgramsInUninstallAreasAreReviewedWithTheAreasNote) {
  clean::testing::FakeEnvironment environment;
  environment.set(L"ProgramFiles", LR"(C:\Program Files)");
  const std::vector<map::Area> areas = {
      area("cache", {LR"(%ProgramFiles%\Cached)"}, map::ReclaimKind::clean_rule),
      area("engine", {LR"(?:\Epic Games\UE_*)"}, map::ReclaimKind::uninstall,
           "remove engine versions in the launcher"),
      area("unity", {LR"(%ProgramFiles%\Unity Hub)", LR"(%UNSET%\Unity)"},
           map::ReclaimKind::uninstall)};
  const std::vector<UninstallEntry> programs = {
      UninstallEntry{.key = "k1", .name = "Unreal 5.3", .location = LR"(D:\Epic Games\UE_5.3)"},
      UninstallEntry{
          .key = "k2", .name = "Unity Hub", .location = LR"(C:\Program Files\Unity Hub)"},
      UninstallEntry{.key = "k3", .name = "Cached", .location = LR"(C:\Program Files\Cached\x)"},
      UninstallEntry{.key = "k4", .name = "Elsewhere", .location = LR"(D:\Epic Games\Launcher)"},
      UninstallEntry{.key = "k5", .name = "No location", .location = {}},
      UninstallEntry{.key = "k6",
                     .name = "Unreal plugin",
                     .location = LR"(c:\epic games\ue_5.4\Engine\Plugins\X)"}};

  const std::vector<ProgramFinding> review = uninstall_review(programs, areas, environment);

  ASSERT_EQ(review.size(), 3U);
  EXPECT_EQ(review[0].program.name, "Unreal 5.3");
  EXPECT_EQ(review[0].area, areas[1].label);
  EXPECT_EQ(review[0].note, "remove engine versions in the launcher");
  EXPECT_EQ(review[1].program.name, "Unity Hub");
  EXPECT_EQ(review[2].program.name, "Unreal plugin");  // case-insensitive, below the area
}

}  // namespace
}  // namespace myclean::scanall
