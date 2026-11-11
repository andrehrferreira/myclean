#include "core/backup/backup.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/backup/repos.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/space/scanner.hpp"
#include "core/store/database.hpp"
#include "space/space_fakes.hpp"

namespace myclean::backup {
namespace {

using Json = nlohmann::json;
using space::testing::FakeDirectoryReader;
using space::testing::folder;
using space::testing::junction;
using std::chrono::days;
namespace fs = std::filesystem;

WallTime date(int year, unsigned month, unsigned day) {
  return std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} /
                               std::chrono::day{day}};
}

const WallTime now = date(2026, 11, 11) + std::chrono::hours{12};

space::ListedEntry file_at(const std::string& name, WallTime modified) {
  space::ListedEntry entry = space::testing::file(name, 1024);
  entry.modified = modified;
  entry.created = modified;
  return entry;
}

space::ListedEntry folder_at(const std::string& name, WallTime modified,
                             std::optional<WallTime> created = std::nullopt) {
  space::ListedEntry entry = folder(name);
  entry.modified = modified;
  entry.created = created.value_or(modified);
  return entry;
}

class FakeSystem final : public IBackupSystem {
 public:
  Result<FileHistoryFacts> history = FileHistoryFacts{};
  Result<bool> versions = false;
  Result<std::vector<RestorePoint>> points = std::vector<RestorePoint>{};
  Result<std::vector<ShadowCopy>> shadows = std::vector<ShadowCopy>{};
  Result<std::vector<OneDriveAccount>> accounts = std::vector<OneDriveAccount>{};
  Result<std::vector<std::string>> programs = std::vector<std::string>{};
  Result<std::vector<Volume>> labels = std::vector<Volume>{{.root = L"C:\\", .label = "SO"},
                                                           {.root = L"K:\\", .label = "Backup 1"},
                                                           {.root = L"L:\\", .label = "Backup 2"}};

  [[nodiscard]] Result<FileHistoryFacts> file_history() const override { return history; }
  [[nodiscard]] Result<bool> image_versions() const override { return versions; }
  [[nodiscard]] Result<std::vector<RestorePoint>> restore_points() const override { return points; }
  [[nodiscard]] Result<std::vector<ShadowCopy>> shadow_copies() const override { return shadows; }
  [[nodiscard]] Result<std::vector<OneDriveAccount>> onedrive() const override { return accounts; }
  [[nodiscard]] Result<std::vector<std::string>> installed_programs() const override {
    return programs;
  }
  [[nodiscard]] Result<std::vector<Volume>> volumes() const override { return labels; }
};

class FakeDrives final : public placement::IDriveFacts {
 public:
  std::vector<placement::TargetVolume> list;
  [[nodiscard]] Result<std::vector<placement::TargetVolume>> volumes() const override {
    return list;
  }
};

placement::TargetVolume hdd(const wchar_t* root, std::uint64_t free_tb, bool healthy = true,
                            std::string note = {}) {
  constexpr std::uint64_t terabyte = 1'000'000'000'000;
  return placement::TargetVolume{.root = root,
                                 .size = Bytes{10 * terabyte},
                                 .free = Bytes{free_tb * terabyte},
                                 .drive_class = placement::DriveClass::hdd,
                                 .healthy = healthy,
                                 .health_note = std::move(note)};
}

class FakeGit final : public IRepoGit {
 public:
  std::map<fs::path, Result<RepoState>> states;
  mutable std::vector<fs::path> asked;
  [[nodiscard]] Result<RepoState> state(const fs::path& work_tree) const override {
    asked.push_back(work_tree);
    const auto found = states.find(work_tree);
    return found == states.end() ? fail(Errc::not_found, "not a repository") : found->second;
  }
};

class BackupTest : public ::testing::Test {
 protected:
  BackupTest() {
    reader_.listings[L"C:\\"] = {};
    reader_.listings[L"K:\\"] = {};
    reader_.listings[L"L:\\"] = {};
    drives_.list = {hdd(L"K:\\", 5), hdd(L"L:\\", 6)};
  }

  Report run() {
    return check(Probes{.system = &system_, .reader = &reader_, .drives = &drives_}, now,
                 std::stop_token{});
  }

  static const Source& source(const Report& report, SourceKind kind) {
    for (const Source& source : report.sources) {
      if (source.kind == kind) {
        return source;
      }
    }
    throw std::runtime_error("no source");
  }

  [[nodiscard]] FakeSystem& system() { return system_; }
  [[nodiscard]] FakeDirectoryReader& reader() { return reader_; }
  [[nodiscard]] FakeDrives& drives() { return drives_; }

 private:
  FakeSystem system_;
  FakeDirectoryReader reader_;
  FakeDrives drives_;
};

// Spec "Every source is reported, found or not".
TEST_F(BackupTest, ReportsEverySourceFoundOrNot) {
  system().history = FileHistoryFacts{
      .configured = true, .target = R"(\\?\Volume{1234}\)", .newest = now - days{3}};
  system().points = std::vector<RestorePoint>{{.created = now - days{9}, .description = "a"},
                                              {.created = now - days{2}, .description = "b"},
                                              {.created = now - days{5}, .description = "c"}};
  const Report report = run();
  ASSERT_EQ(report.sources.size(), 7U);
  EXPECT_EQ(source(report, SourceKind::file_history).status, Status::found);
  EXPECT_EQ(source(report, SourceKind::file_history).location, R"(\\?\Volume{1234}\)");
  EXPECT_EQ(source(report, SourceKind::system_image).status, Status::not_found);
  const Source& points = source(report, SourceKind::restore_points);
  EXPECT_EQ(points.status, Status::found);
  EXPECT_EQ(points.copies, 3U);
  EXPECT_EQ(points.newest, now - days{2});
  EXPECT_FALSE(points.counts());
  EXPECT_EQ(source(report, SourceKind::shadow_copies).status, Status::not_found);
  EXPECT_EQ(source(report, SourceKind::onedrive).status, Status::not_found);
  ASSERT_TRUE(report.last.has_value());
  EXPECT_EQ(report.last.value_or(LastBackup{}).source, SourceKind::file_history);
  const Json json = report_json(report, now);
  EXPECT_EQ(json.at("summary"), "newest backup is 3 days old (File History)");
  EXPECT_EQ(json.at("last_backup").at("age_days"), 3);
  EXPECT_EQ(json.at("sources").size(), 7U);
  EXPECT_TRUE(json.at("recommendation").is_null());
}

// Spec "Unreadable source is reported as unknown".
TEST_F(BackupTest, AnUnreadableSourceCouldNotBeChecked) {
  system().versions = fail(Errc::access_denied, "wbadmin get versions needs administrator rights");
  system().points = fail(Errc::access_denied, "SystemRestore needs administrator rights");
  const Report report = run();
  const Source& image = source(report, SourceKind::system_image);
  EXPECT_EQ(image.status, Status::unknown);
  EXPECT_EQ(image.detail, "wbadmin get versions needs administrator rights");
  EXPECT_EQ(source(report, SourceKind::restore_points).status, Status::unknown);
  const Json json = report_json(report, now);
  EXPECT_EQ(json.at("sources")[1].at("status"), "could_not_check");
}

TEST_F(BackupTest, AnImageFolderIsASystemImageDatedByItsNewestSet) {
  reader().listings[L"L:\\"] = {folder_at("WindowsImageBackup", now - days{60})};
  reader().listings[L"L:\\WindowsImageBackup"] = {folder_at("BOLADO", now - days{60})};
  reader().listings[L"L:\\WindowsImageBackup\\BOLADO"] = {
      folder_at("Backup 2026-10-01 120000", now - days{41}), folder_at("Catalog", now - days{40})};
  const Report report = run();
  const Source& image = source(report, SourceKind::system_image);
  EXPECT_EQ(image.status, Status::found);
  EXPECT_EQ(image.newest, now - days{40});
  EXPECT_EQ(image.location, "L:\\WindowsImageBackup");
  // Not reported a second time as an artifact of the backup drive.
  EXPECT_EQ(source(report, SourceKind::backup_drives).status, Status::not_found);
}

TEST_F(BackupTest, WbadminVersionsWithoutAFolderAreAnImageOfUnknownTime) {
  system().versions = true;
  const Report report = run();
  const Source& image = source(report, SourceKind::system_image);
  EXPECT_EQ(image.status, Status::found);
  EXPECT_FALSE(image.newest);
  EXPECT_TRUE(report.backup_exists);
  EXPECT_FALSE(report.last);
}

// Spec "Macrium image on L:".
TEST_F(BackupTest, FindsAThirdPartyToolAndItsArtifacts) {
  system().programs =
      std::vector<std::string>{"Macrium Reflect Free", "7-Zip 23.01 (x64)", "Arqade Viewer"};
  reader().listings[L"L:\\"] = {folder_at("Images", now - days{30})};
  reader().listings[L"L:\\Images"] = {file_at("C-drive-00-00.mrimg", date(2026, 10, 12)),
                                      file_at("C-drive-01-01.mrimg", date(2026, 9, 12)),
                                      file_at("notes.txt", date(2026, 10, 20))};
  const Report report = run();
  const Source& tools = source(report, SourceKind::third_party);
  EXPECT_EQ(tools.status, Status::found);
  EXPECT_EQ(tools.names, std::vector<std::string>{"Macrium Reflect Free"});
  const Source& drives = source(report, SourceKind::backup_drives);
  ASSERT_EQ(drives.artifacts.size(), 2U);
  EXPECT_EQ(drives.artifacts.front().kind, ArtifactKind::macrium_image);
  EXPECT_EQ(drives.artifacts.front().path, fs::path(L"L:\\Images\\C-drive-00-00.mrimg"));
  EXPECT_EQ(drives.newest, date(2026, 10, 12));
  ASSERT_TRUE(report.last.has_value());
  EXPECT_EQ(report.last.value_or(LastBackup{}).name, "Macrium Reflect image");
  EXPECT_EQ(report.last.value_or(LastBackup{}).location, "L:\\Images\\C-drive-00-00.mrimg");
}

// Spec "Junction on a backup drive is not followed".
TEST_F(BackupTest, AJunctionOnABackupDriveIsNotFollowed) {
  reader().listings[L"K:\\"] = {junction("Old"), folder_at("Images", now)};
  reader().listings[L"K:\\Images"] = {};
  static_cast<void>(run());
  EXPECT_FALSE(reader().was_listed(L"K:\\Old"));
  EXPECT_TRUE(reader().was_listed(L"K:\\Images"));
}

TEST_F(BackupTest, ManualCopiesAreDatedByTheirNameAndNotEntered) {
  reader().listings[L"K:\\"] = {
      folder_at("Backup-20-09-2026", date(2026, 3, 27), date(2026, 9, 20)),
      folder_at("Assets", date(2026, 10, 1))};
  reader().listings[L"K:\\Assets"] = {file_at("old.vhdx", date(2026, 10, 1)),
                                      file_at("db.wim", date(2026, 10, 1))};
  reader().listings[L"L:\\"] = {folder_at("Backup-24-09-2026", date(2026, 9, 25))};
  reader().listings[L"L:\\Backup-24-09-2026"] = {
      file_at("prod-20260925T022441Z.dump", date(2026, 9, 25))};
  const Report report = run();
  const Source& drives = source(report, SourceKind::backup_drives);
  ASSERT_EQ(drives.artifacts.size(), 2U);  // no .vhdx or .wim, and the copies are not entered
  EXPECT_FALSE(reader().was_listed(L"L:\\Backup-24-09-2026"));
  EXPECT_EQ(drives.artifacts[0].path, fs::path(L"L:\\Backup-24-09-2026"));
  EXPECT_EQ(drives.artifacts[0].newest, date(2026, 9, 24));
  EXPECT_EQ(drives.artifacts[1].kind, ArtifactKind::manual_copy);
  EXPECT_EQ(drives.artifacts[1].newest, date(2026, 9, 20));
  ASSERT_TRUE(report.last.has_value());
  EXPECT_EQ(report.last.value_or(LastBackup{}).name, "manual copy");
  EXPECT_EQ(report_json(report, now).at("summary"), "newest backup is 48 days old (manual copy)");
}

// Spec "Age names its source".
TEST_F(BackupTest, TheAgeIsTheNewestCopyAndNamesItsSource) {
  system().history = FileHistoryFacts{.configured = true, .target = "K:", .newest = now - days{3}};
  reader().listings[L"L:\\"] = {file_at("image.mrimg", now - days{19})};
  const Report report = run();
  ASSERT_TRUE(report.last.has_value());
  EXPECT_EQ(report.last.value_or(LastBackup{}).source, SourceKind::file_history);
  const std::vector<alerts::Reading> readings = age_readings(report, now);
  ASSERT_EQ(readings.size(), 1U);
  EXPECT_EQ(readings[0].metric, age_metric);
  EXPECT_EQ(readings[0].value, 3.0);
  EXPECT_EQ(readings[0].detail, "newest backup is 3 days old (File History)");
}

// Spec "Restore points alone are not a backup" and "Recommendation without action".
TEST_F(BackupTest, RestorePointsAloneAreNoBackupAndTheRecommendationNamesHealthyDrives) {
  system().points = std::vector<RestorePoint>{{.created = now - days{1}, .description = "daily"}};
  system().shadows = std::vector<ShadowCopy>{{.volume = "C:", .created = now - days{1}}};
  const Report report = run();
  EXPECT_EQ(source(report, SourceKind::restore_points).status, Status::found);
  EXPECT_FALSE(report.backup_exists);
  EXPECT_FALSE(report.last);
  ASSERT_TRUE(report.recommendation.has_value());
  EXPECT_EQ(report.recommendation.value_or(Recommendation{}).named,
            (std::vector<std::string>{"L:", "K:"}));
  EXPECT_NE(report.recommendation.value_or(Recommendation{}).text.find("File History to L:"),
            std::string::npos);
  EXPECT_NE(report.recommendation.value_or(Recommendation{}).text.find("system image to K:"),
            std::string::npos);
  const Json json = report_json(report, now);
  EXPECT_EQ(json.at("summary"), "no backup exists");
  const std::vector<alerts::Reading> readings = age_readings(report, now);
  ASSERT_EQ(readings.size(), 1U);
  EXPECT_TRUE(std::isinf(readings[0].value));
  EXPECT_EQ(readings[0].detail, "no backup exists");
}

// Spec "Degraded drive is not recommended".
TEST_F(BackupTest, ADegradedDriveIsNotRecommended) {
  drives().list = {
      hdd(L"K:\\", 5), hdd(L"L:\\", 6, false, "1 new reallocated sector"),
      placement::TargetVolume{
          .root = L"C:\\", .drive_class = placement::DriveClass::nvme, .healthy = true}};
  const Report report = run();
  ASSERT_TRUE(report.recommendation.has_value());
  EXPECT_EQ(report.recommendation.value_or(Recommendation{}).named, std::vector<std::string>{"K:"});
  ASSERT_EQ(report.recommendation.value_or(Recommendation{}).left_out.size(), 1U);
  EXPECT_EQ(report.recommendation.value_or(Recommendation{}).left_out[0].drive, "L:");
  EXPECT_NE(report.recommendation.value_or(Recommendation{}).left_out[0].reason.find("reallocated"),
            std::string::npos);
  EXPECT_EQ(report.recommendation.value_or(Recommendation{}).text.find("L:"), std::string::npos);
}

TEST_F(BackupTest, OneDriveFolderBackupIsABackupWithoutACopyTime) {
  system().accounts = std::vector<OneDriveAccount>{{.account = "Personal",
                                                    .folder = L"C:\\Users\\owner\\OneDrive",
                                                    .protected_folders = {"Desktop", "Documents"},
                                                    .sign_in_error = date(2025, 10, 18)}};
  const Report report = run();
  const Source& onedrive = source(report, SourceKind::onedrive);
  EXPECT_EQ(onedrive.status, Status::found);
  EXPECT_NE(onedrive.detail.find("sign-in error"), std::string::npos);
  EXPECT_TRUE(report.backup_exists);
  EXPECT_FALSE(report.recommendation);
  EXPECT_TRUE(age_readings(report, now).empty());
  EXPECT_EQ(report_json(report, now).at("sources")[4].at("folders"),
            (Json::array({"Desktop", "Documents"})));
}

TEST(NameDateTest, ReadsTheDatesOfBackupFolderNames) {
  EXPECT_EQ(name_date("Backup-24-09-2026"), date(2026, 9, 24));
  EXPECT_EQ(name_date("backup_2026-09-24"), date(2026, 9, 24));
  EXPECT_EQ(name_date("Backup 20260924"), date(2026, 9, 24));
  EXPECT_EQ(name_date("Backup Git"), std::nullopt);
  EXPECT_EQ(name_date("Backup-31-02-2026"), std::nullopt);
  EXPECT_EQ(name_date("Backup 1"), std::nullopt);
}

TEST(BackupProductTest, MatchesKnownProductsOnly) {
  EXPECT_EQ(backup_product("Macrium Reflect Workstation"), "Macrium Reflect");
  EXPECT_EQ(backup_product("Veeam Agent for Microsoft Windows"), "Veeam Agent");
  EXPECT_EQ(backup_product("Arq 7"), "Arq");
  EXPECT_EQ(backup_product("Arqade"), std::nullopt);
  EXPECT_EQ(backup_product("Microsoft OneDrive"), std::nullopt);
}

// Spec "Three kinds of unprotected work" and "Check makes no network call" (the check only asks
// IRepoGit, whose implementation reads local refs).
TEST(RepositoryCheckTest, ReportsTheThreeKindsOfUnprotectedWork) {
  FakeDirectoryReader reader;
  reader.listings[L"F:\\"] = {
      folder("A"), folder("B"), folder("C"), folder("D"), folder("node_modules"), junction("Link")};
  reader.listings[L"F:\\A"] = {folder(".git")};
  reader.listings[L"F:\\B"] = {folder(".git"), folder("src")};
  reader.listings[L"F:\\C"] = {space::testing::file(".git", 40)};  // a linked worktree
  reader.listings[L"F:\\D"] = {folder("E")};
  reader.listings[L"F:\\D\\E"] = {folder(".git")};
  FakeGit git;
  git.states[L"F:\\A"] = RepoState{.remotes = 0, .unpushed = 12};
  git.states[L"F:\\B"] = RepoState{.remotes = 1, .unpushed = 4};
  git.states[L"F:\\C"] = RepoState{.remotes = 1, .uncommitted = true};
  git.states[L"F:\\D\\E"] = RepoState{.remotes = 1};
  const std::vector<fs::path> roots = {L"F:\\", L"W:\\"};
  const std::vector<std::wstring> skip = {L"NODE_MODULES"};
  const RepoWalk walk = find_repositories(reader, roots, skip, {}, std::stop_token{});
  EXPECT_EQ(walk.repositories, (std::vector<fs::path>{L"F:\\A", L"F:\\B", L"F:\\C", L"F:\\D\\E"}));
  EXPECT_FALSE(reader.was_listed(L"F:\\B\\src"));
  EXPECT_FALSE(reader.was_listed(L"F:\\node_modules"));
  EXPECT_FALSE(reader.was_listed(L"F:\\Link"));
  const RepoReport report = check_repositories(git, walk.repositories, std::stop_token{});
  EXPECT_EQ(report.checked, 4U);
  ASSERT_EQ(report.unprotected.size(), 3U);
  EXPECT_EQ(report.unprotected[0].text, "no remote");
  EXPECT_EQ(report.unprotected[1].text, "4 unpushed commits");
  EXPECT_EQ(report.unprotected[2].text, "uncommitted changes");
  EXPECT_EQ(report.unprotected[2].path, fs::path(L"F:\\C"));
}

TEST(RepositoryCheckTest, AFailingRepositoryIsAProblemNotAFinding) {
  const FakeGit git;
  const std::vector<fs::path> repositories = {L"F:\\Broken"};
  const RepoReport report = check_repositories(git, repositories, std::stop_token{});
  EXPECT_EQ(report.checked, 0U);
  EXPECT_TRUE(report.unprotected.empty());
  ASSERT_EQ(report.problems.size(), 1U);
  EXPECT_NE(report.problems[0].find("F:\\Broken"), std::string::npos);
}

}  // namespace
}  // namespace myclean::backup
