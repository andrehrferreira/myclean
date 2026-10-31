#include "core/map/disk_map.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/clean/probes.hpp"
#include "core/map/attribution.hpp"
#include "core/map/json.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/space/scanner.hpp"
#include "space/space_fakes.hpp"

namespace myclean::map {
namespace {

using space::testing::file;
using space::testing::folder;
using space::testing::scan_ok;
using namespace myclean::literals;

class Environment final : public clean::IEnvironment {
 public:
  std::map<std::wstring, std::wstring, std::less<>> values;

  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view name) const override {
    const auto found = values.find(name);
    return found == values.end() ? std::nullopt : std::optional{found->second};
  }
};

Area area(std::string id, std::vector<std::wstring> paths, Category category,
          ReclaimKind kind = ReclaimKind::none, std::string ref = {}) {
  return Area{.id = std::move(id),
              .paths = std::move(paths),
              .category = category,
              .label = "label",
              .reclaim = Reclaim{.kind = kind, .ref = std::move(ref)}};
}

DiskMap build(const space::ScanResult& scan, std::vector<Area> areas,
              const clean::IEnvironment& environment, std::optional<VolumeSpace> volume = {},
              MapOptions options = {.min_size = 1_B, .depth = 10}) {
  Result<DiskMap> map = build_map(scan, std::move(areas), environment, volume, options);
  if (!map) {
    throw std::runtime_error(map.error().message);
  }
  return std::move(*map);
}

const Match& match_at(const DiskMap& map, const std::filesystem::path& path) {
  for (const Match& match : map.matches) {
    if (match.path == path) {
      return match;
    }
  }
  throw std::runtime_error("no match at " + space::utf8(path));
}

std::string area_id(const DiskMap& map, const std::filesystem::path& path) {
  return map.areas[match_at(map, path).area].id;
}

// C:\ with Windows (WinSxS with a hard link to System32), a profile and a hibernation file.
void fill(space::testing::FakeDirectoryReader& reader) {
  reader.listings[R"(C:\)"] = {folder("Windows"), folder("Users"), folder("Stuff"),
                               file("hiberfil.sys", 5000, 1)};
  reader.listings[R"(C:\Windows)"] = {folder("System32"), folder("WinSxS"), file("a.dll", 100, 2)};
  reader.listings[R"(C:\Windows\System32)"] = {file("kernel.dll", 400, 3)};
  reader.listings[R"(C:\Windows\WinSxS)"] = {file("kernel.dll", 400, 3), file("old.dll", 50, 4)};
  reader.listings[R"(C:\Users)"] = {folder("me"), folder("other")};
  reader.listings[R"(C:\Users\me)"] = {folder("AppData"), folder("Videos"), file("x.txt", 7, 5)};
  reader.listings[R"(C:\Users\me\AppData)"] = {folder("Local")};
  reader.listings[R"(C:\Users\me\AppData\Local)"] = {folder("Temp"), folder("Packages")};
  reader.listings[R"(C:\Users\me\AppData\Local\Temp)"] = {file("t.tmp", 300, 6)};
  reader.listings[R"(C:\Users\me\AppData\Local\Packages)"] = {folder("Ubuntu_1")};
  reader.listings[R"(C:\Users\me\AppData\Local\Packages\Ubuntu_1)"] = {folder("LocalState")};
  reader.listings[R"(C:\Users\me\AppData\Local\Packages\Ubuntu_1\LocalState)"] = {
      file("ext4.vhdx", 9000, 7), file("state.db", 20, 8)};
  reader.listings[R"(C:\Users\me\Videos)"] = {file("film.mp4", 2000, 9)};
  reader.errors.emplace(R"(C:\Users\other)",
                        Error{.code = Errc::access_denied, .message = "denied"});
  reader.listings[R"(C:\Stuff)"] = {folder("Deep"), file("s.bin", 60, 10)};
  reader.listings[R"(C:\Stuff\Deep)"] = {file("d.bin", 600, 11)};
}

// The fake volume and the variables of its user.
struct Machine {
  Machine() {
    fill(reader);
    environment.values = {{L"WINDIR", LR"(C:\Windows)"},
                          {L"USERPROFILE", LR"(C:\Users\me)"},
                          {L"LOCALAPPDATA", LR"(C:\Users\me\AppData\Local)"},
                          {L"TEMP", LR"(C:\Users\me\AppData\Local\Temp)"}};
  }

  space::testing::FakeDirectoryReader reader;
  Environment environment;
};

std::vector<Area> areas() {
  return {
      area("windows", {LR"(%WINDIR%)"}, Category::system),
      area("winsxs", {LR"(%WINDIR%\WinSxS)"}, Category::windows_servicing, ReclaimKind::clean_rule,
           "windows.component_store"),
      area("hibernation", {LR"(?:\hiberfil.sys)"}, Category::page_hibernate, ReclaimKind::setting,
           "hibernation"),
      area("store-apps", {LR"(%LOCALAPPDATA%\Packages)"}, Category::apps,
           ReclaimKind::manual_review),
      area("wsl", {LR"(%LOCALAPPDATA%\Packages\*\LocalState\*.vhdx)"}, Category::vm_disks,
           ReclaimKind::relocate, "wsl"),
      area("temp", {LR"(%TEMP%)"}, Category::app_caches, ReclaimKind::clean_rule,
           "windows.temp.user"),
      area("media", {LR"(%USERPROFILE%\Videos)"}, Category::user_media, ReclaimKind::manual_review),
      area("other-profiles", {LR"(C:\Users\other)"}, Category::other),
      area("unset", {LR"(%NOT_SET%\Stuff)"}, Category::other)};
}

TEST(BuildMap, TheMostPreciseLocationWins) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment);
  EXPECT_EQ(area_id(map, R"(C:\Windows)"), "windows");
  EXPECT_EQ(area_id(map, R"(C:\Windows\WinSxS)"), "winsxs");
  EXPECT_EQ(area_id(map, R"(C:\hiberfil.sys)"), "hibernation");
  EXPECT_TRUE(match_at(map, R"(C:\hiberfil.sys)").file);
  // A file location inside a folder location takes the file's bytes from the folder's area.
  EXPECT_EQ(area_id(map, R"(C:\Users\me\AppData\Local\Packages)"), "store-apps");
  EXPECT_EQ(area_id(map, R"(C:\Users\me\AppData\Local\Packages\Ubuntu_1\LocalState\ext4.vhdx)"),
            "wsl");
  EXPECT_EQ(match_at(map, R"(C:\Users\me\AppData\Local\Packages)").size, 20_B);
  // A location with a variable that is not set names nothing.
  EXPECT_THROW(static_cast<void>(match_at(map, R"(C:\Stuff)")), std::runtime_error);
}

TEST(BuildMap, EquallyPreciseLocationsGoToTheFirstInTheFile) {
  const Machine machine;
  std::vector<Area> list = areas();
  list.insert(list.begin(), area("first", {LR"(C:\Win*)"}, Category::other));
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), list, machine.environment);
  EXPECT_EQ(area_id(map, R"(C:\Windows)"), "first");
  EXPECT_EQ(area_id(map, R"(C:\Windows\WinSxS)"), "winsxs");
}

TEST(BuildMap, CountsEveryByteOnceAndSplitsExplainedFromUnexplained) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment);
  // 5000 + 100 + 400 + 50 + 7 + 300 + 9000 + 20 + 2000 + 60 + 600; the hard link counts once.
  EXPECT_EQ(map.scanned, 17537_B);
  EXPECT_EQ(map.apparent, 17937_B);
  EXPECT_EQ(map.unexplained, 667_B);  // x.txt and C:\Stuff
  EXPECT_EQ(map.explained + map.unexplained, map.scanned);
  Bytes categories;
  for (const CategoryTotal& category : map.categories) {
    categories += category.size;
  }
  EXPECT_EQ(categories, map.explained);
  // setting 5000 + relocate 9000 + clean rules 50 (WinSxS without the shared link) + 300.
  EXPECT_EQ(map.reclaimable, 14350_B);
  EXPECT_EQ(map.review, 2020_B);  // videos and the store app's own file
  EXPECT_EQ(map.categories.front().category, Category::vm_disks);
  EXPECT_EQ(map.categories.front().reclaimable, 9000_B);
}

TEST(BuildMap, HardLinksAreRealOnceAndApparentEverywhere) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment);
  const Match& winsxs = match_at(map, R"(C:\Windows\WinSxS)");
  EXPECT_EQ(winsxs.size, 50_B);
  EXPECT_EQ(winsxs.apparent, 450_B);
  EXPECT_EQ(match_at(map, R"(C:\Windows)").size, 500_B);  // a.dll and System32
}

TEST(BuildMap, ReportsWhatTheScanCouldNotSee) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment,
                            VolumeSpace{.total = 100'000_B, .free = 80'000_B});
  EXPECT_TRUE(map.volume_root);
  ASSERT_TRUE(map.not_visible.has_value());
  EXPECT_EQ(*map.not_visible, 20'000 - 17537);
  ASSERT_EQ(map.unreadable.size(), 1U);
  EXPECT_EQ(map.unreadable.front().path, std::filesystem::path(R"(C:\Users\other)"));
  EXPECT_EQ(map.unreadable.front().error.code, Errc::access_denied);
  ASSERT_TRUE(map.unreadable.front().match.has_value());
  EXPECT_EQ(map.areas[map.matches[*map.unreadable.front().match].area].id, "other-profiles");
  EXPECT_EQ(match_at(map, R"(C:\Users\other)").unreadable, 1U);
}

TEST(BuildMap, AFolderScanInheritsTheAreaAboveItAndHasNoVisibilityGap) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\Windows\WinSxS)"), areas(),
                            machine.environment, VolumeSpace{.total = 100'000_B, .free = 80'000_B});
  EXPECT_FALSE(map.volume_root);
  EXPECT_FALSE(map.not_visible.has_value());
  EXPECT_EQ(area_id(map, R"(C:\Windows\WinSxS)"), "winsxs");
  EXPECT_TRUE(map.tree.matched_here);
  EXPECT_EQ(map.explained, map.scanned);
}

TEST(BuildMap, TheTreeKeepsWhatReachesTheThresholdAndTheDepth) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment,
                            std::nullopt, {.min_size = 600_B, .depth = 2});
  std::vector<std::filesystem::path> top;
  top.reserve(map.tree.children.size());
  for (const Node& child : map.tree.children) {
    top.push_back(child.path);
  }
  // Largest first: Users 11327, hiberfil.sys 5000, Stuff 660; Windows (550) is below 600.
  EXPECT_EQ(top, (std::vector<std::filesystem::path>{R"(C:\Users)", R"(C:\hiberfil.sys)",
                                                     R"(C:\Stuff)"}));
  const Node& users = map.tree.children.front();
  ASSERT_EQ(users.children.size(), 1U);                  // me; other could not be read
  EXPECT_TRUE(users.children.front().children.empty());  // depth 2 ends at C:\Users\me
  EXPECT_FALSE(users.match.has_value());
  EXPECT_EQ(users.unexplained, 7_B);
  const Node& hiberfil = map.tree.children[1];
  EXPECT_TRUE(hiberfil.file);
  EXPECT_TRUE(hiberfil.matched_here);
  const Node& stuff = map.tree.children[2];
  ASSERT_EQ(stuff.children.size(), 1U);  // Deep (600); s.bin (60) is below the threshold
  EXPECT_EQ(stuff.unexplained, 660_B);
}

TEST(BuildMap, UnexplainedFoldersAreTheDeepestThatReachTheThreshold) {
  const Machine machine;
  const DiskMap small = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment,
                              std::nullopt, {.min_size = 600_B, .depth = 1});
  ASSERT_EQ(small.unexplained_folders.size(), 1U);
  EXPECT_EQ(small.unexplained_folders.front().path, std::filesystem::path(R"(C:\Stuff\Deep)"));
  const DiskMap large = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment,
                              std::nullopt, {.min_size = 650_B, .depth = 1});
  ASSERT_EQ(large.unexplained_folders.size(), 1U);
  EXPECT_EQ(large.unexplained_folders.front().path, std::filesystem::path(R"(C:\Stuff)"));
  EXPECT_EQ(large.unexplained_folders.front().unexplained, 660_B);
}

TEST(BuildMap, MatchesAreLargestFirstAndTheJsonNamesAreasById) {
  const Machine machine;
  const DiskMap map = build(scan_ok(machine.reader, R"(C:\)"), areas(), machine.environment);
  ASSERT_GE(map.matches.size(), 3U);
  EXPECT_EQ(
      map.matches.front().path,
      std::filesystem::path(R"(C:\Users\me\AppData\Local\Packages\Ubuntu_1\LocalState\ext4.vhdx)"));
  EXPECT_EQ(map.area_totals.front().size, 9000_B);
  const nlohmann::json json = map;
  EXPECT_EQ(json.at("matches").at(0).at("area"), "wsl");
  EXPECT_EQ(json.at("areas").at(0).at("reclaim").at("kind"), "relocate");
  EXPECT_EQ(json.at("unreadable").at(0).at("area"), "other-profiles");
  EXPECT_EQ(json.at("explained").get<std::uint64_t>() + json.at("unexplained").get<std::uint64_t>(),
            json.at("scanned").get<std::uint64_t>());
  EXPECT_TRUE(json.at("not_visible").is_null());
}

TEST(BuildMap, RejectsANegativeDepth) {
  const Machine machine;
  const Result<DiskMap> map = build_map(scan_ok(machine.reader, R"(C:\)"), areas(),
                                        machine.environment, std::nullopt, {.depth = -1});
  ASSERT_FALSE(map.has_value());
  EXPECT_EQ(map.error().code, Errc::invalid_argument);
}

// The shipped data/attribution.toml explains the large places measured on the owner's C:.
TEST(ShippedAttribution, ExplainsTheLargePlacesOfAWorkstation) {
  struct Expected {
    Expected(std::wstring path, std::string area, Category category, ReclaimKind kind)
        : path(std::move(path)), area(std::move(area)), category(category), kind(kind) {}

    std::wstring path;  // below C:\Users\me unless absolute
    std::string area;
    Category category;
    ReclaimKind kind;
  };
  const std::vector<Expected> expected = {
      {LR"(C:\hiberfil.sys)", "windows.hibernation", Category::page_hibernate,
       ReclaimKind::setting},
      {LR"(C:\pagefile.sys)", "windows.page_file", Category::page_hibernate, ReclaimKind::setting},
      {LR"(C:\Windows\WinSxS)", "windows.component_store", Category::windows_servicing,
       ReclaimKind::clean_rule},
      {LR"(C:\Windows\Panther)", "windows.setup_logs", Category::windows_servicing,
       ReclaimKind::clean_rule},
      {LR"(C:\Windows\assembly)", "windows.assembly", Category::system, ReclaimKind::none},
      {LR"(C:\$Recycle.Bin)", "windows.recycle_bin", Category::recycle_bin,
       ReclaimKind::clean_rule},
      {LR"(C:\Program Files\App)", "apps.program_files", Category::apps, ReclaimKind::uninstall},
      {LR"(C:\vcpkg\buildtrees)", "dev.vcpkg_build", Category::dev_caches,
       ReclaimKind::manual_review},
      {LR"(C:\Users\Public\Documents)", "user.documents", Category::user_documents,
       ReclaimKind::manual_review},
      {LR"(AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_x\LocalState\ext4.vhdx)",
       "dev.wsl", Category::vm_disks, ReclaimKind::relocate},
      {LR"(AppData\Local\Packages\CanonicalGroupLimited.Ubuntu24.04LTS_x\rootfs)", "dev.wsl",
       Category::vm_disks, ReclaimKind::relocate},
      {LR"(AppData\Local\Docker\wsl)", "dev.docker", Category::vm_disks, ReclaimKind::relocate},
      {LR"(AppData\Local\Packages\PythonSoftwareFoundation.Python.3.13_x)", "dev.python_store",
       Category::dev_toolchains, ReclaimKind::uninstall},
      {LR"(AppData\Local\zig)", "dev.zig_cache", Category::dev_caches, ReclaimKind::manual_review},
      {LR"(AppData\Local\UzPainter)", "apps.uzpainter", Category::apps, ReclaimKind::manual_review},
      {LR"(AppData\Local\UzPainter\cache)", "apps.uzpainter_cache", Category::app_caches,
       ReclaimKind::manual_review},
      {LR"(.codex)", "apps.ai_cli_history", Category::apps, ReclaimKind::manual_review},
      {LR"(.claude)", "apps.ai_cli_history", Category::apps, ReclaimKind::manual_review},
      {LR"(AppData\Local\Google\Chrome\User Data\OptGuideOnDeviceModel)", "browser.chrome_ai_model",
       Category::browser_caches, ReclaimKind::manual_review},
      {LR"(AppData\Local\Google\Chrome\User Data\Default\Cache)", "browser.chrome_cache",
       Category::browser_caches, ReclaimKind::clean_rule},
      {LR"(AppData\Local\Google\Chrome\User Data\Default\History)", "browser.profiles",
       Category::apps, ReclaimKind::manual_review},
      {LR"(AppData\Roaming\Code\WebStorage)", "dev.vscode_web_storage", Category::dev_caches,
       ReclaimKind::manual_review},
      {LR"(AppData\Roaming\Code\CachedExtensionVSIXs)", "dev.vscode_vsix_cache",
       Category::app_caches, ReclaimKind::manual_review},
      {LR"(.rustup)", "dev.rustup", Category::dev_toolchains, ReclaimKind::manual_review},
      {LR"(.cargo)", "dev.cargo", Category::dev_caches, ReclaimKind::relocate},
      {LR"(.gradle)", "dev.gradle", Category::dev_caches, ReclaimKind::relocate},
      {LR"(.android\avd)", "dev.android_avd", Category::vm_disks, ReclaimKind::manual_review},
      {LR"(AppData\Local\Android\Sdk)", "dev.android_sdk", Category::dev_toolchains,
       ReclaimKind::manual_review},
      {LR"(AppData\Local\vcpkg\archives)", "dev.vcpkg_build", Category::dev_caches,
       ReclaimKind::manual_review},
      {LR"(.elyriu-local-ai)", "dev.ai_models", Category::dev_caches, ReclaimKind::manual_review},
      {LR"(AppData\Local\ms-playwright)", "dev.playwright", Category::dev_caches,
       ReclaimKind::manual_review},
      {LR"(AppData\Local\Temp)", "apps.user_temp", Category::app_caches, ReclaimKind::clean_rule},
      {LR"(Downloads)", "user.downloads", Category::downloads, ReclaimKind::manual_review},
  };
  const std::filesystem::path profile = LR"(C:\Users\me)";
  space::testing::FakeDirectoryReader reader;
  std::uint8_t id = 0;
  for (const Expected& entry : expected) {
    const std::filesystem::path path =
        entry.path.starts_with(L"C:") ? std::filesystem::path(entry.path) : profile / entry.path;
    // Each place holds one file; every folder above it is listed.
    reader.listings[path].push_back(file("content.bin", 1000, ++id));
    for (std::filesystem::path child = path; child.has_relative_path();
         child = child.parent_path()) {
      auto& listing = reader.listings[child.parent_path()];
      const bool listed = std::ranges::any_of(listing, [&child](const space::ListedEntry& entry) {
        return entry.name == child.filename();
      });
      if (!listed) {
        listing.push_back(folder(space::utf8(child.filename())));
      }
    }
  }
  Environment environment;
  environment.values = {{L"WINDIR", LR"(C:\Windows)"},
                        {L"SystemDrive", L"C:"},
                        {L"ProgramFiles", LR"(C:\Program Files)"},
                        {L"ProgramFiles(x86)", LR"(C:\Program Files (x86))"},
                        {L"ProgramData", LR"(C:\ProgramData)"},
                        {L"PUBLIC", LR"(C:\Users\Public)"},
                        {L"USERPROFILE", profile.native()},
                        {L"APPDATA", (profile / LR"(AppData\Roaming)").native()},
                        {L"LOCALAPPDATA", (profile / LR"(AppData\Local)").native()},
                        {L"TEMP", (profile / LR"(AppData\Local\Temp)").native()}};
  const rules::DiskDataFiles files;
  Result<std::vector<Area>> shipped = load_attribution(files, MYCLEAN_DATA_DIR);
  ASSERT_TRUE(shipped.has_value()) << shipped.error().message;
  const DiskMap map = build(scan_ok(reader, R"(C:\)"), std::move(*shipped), environment);
  EXPECT_EQ(map.unexplained, Bytes{});
  for (const Expected& entry : expected) {
    const std::filesystem::path path =
        entry.path.starts_with(L"C:") ? std::filesystem::path(entry.path) : profile / entry.path;
    const std::filesystem::path content = path / "content.bin";
    const Match* found = nullptr;
    for (const Match& match : map.matches) {
      // The match holding the file: the most precise one whose path contains it.
      if (content.native().starts_with(match.path.native() + L"\\") &&
          (found == nullptr || match.path.native().size() > found->path.native().size())) {
        found = &match;
      }
    }
    ASSERT_NE(found, nullptr) << space::utf8(path);
    const Area& area = map.areas[found->area];
    EXPECT_EQ(area.id, entry.area) << space::utf8(path);
    EXPECT_EQ(area.category, entry.category) << space::utf8(path);
    EXPECT_EQ(area.reclaim.kind, entry.kind) << space::utf8(path);
  }
}

}  // namespace
}  // namespace myclean::map
