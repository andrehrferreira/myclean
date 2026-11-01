// SafeRemove over the real Win32 file system, inside the sandbox. The protected folders of
// the deny-list (profiles, Documents, MyClean data) are sandbox folders here, so even a
// failure could not reach the owner's real files.
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/plan_source.hpp"
#include "core/safety/safe_remove.hpp"
#include "platform/fs/win32_file_system.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

using std::filesystem::path;

class OnePlan final : public safety::IPlanSource {
 public:
  OnePlan() = default;

  [[nodiscard]] Result<Plan> load(PlanId id) const override {
    if (id != plan.id) {
      return fail(Errc::not_found, "no such plan");
    }
    return plan;
  }

  Plan plan{.id = PlanId{1}, .proposer = Actor::scheduler(), .state = PlanState::applying};
};

class SafeRemoveOnDisk : public ::testing::Test {
 protected:
  SafeRemoveOnDisk() {
    write_file(documents_ / L"thesis.docx", 100);
    std::filesystem::create_directories(root_ / L"ProgramData\\MyClean");
  }

  PlanItemId add_item(const path& item) {
    const PlanItemId id{static_cast<std::int64_t>(plans_.plan.items.size()) + 1};
    plans_.plan.items.push_back(
        PlanItem{.id = id, .path = item, .identity = fs_.stat(item).value().identity});
    return id;
  }

  safety::ItemOutcome remove(PlanItemId item) {
    const std::vector<PlanItemId> items = {item};
    return remover().remove(plans_.plan.id, items).front();
  }

  safety::ItemOutcome move(PlanItemId item, const path& destination) {
    const std::vector<safety::MoveRequest> requests = {{.item = item, .destination = destination}};
    return remover().move(plans_.plan.id, requests).front();
  }

  safety::SafeRemove remover() {
    const safety::ProtectedLocations locations{
        .windows = path{L"C:\\Windows"},
        .profiles = root_ / L"Users",
        .owner_profile = root_ / L"Users\\owner",
        .owner_known_folders = {documents_},
        .myclean = {root_ / L"ProgramData\\MyClean"},
    };
    return safety::SafeRemove::create(fs_, plans_, locations).value();
  }

  Win32FileSystem& fs() { return fs_; }

  [[nodiscard]] const path& root() const { return root_; }
  [[nodiscard]] const path& documents() const { return documents_; }

 private:
  Sandbox sandbox_;
  path root_ = sandbox_.root();
  path documents_ = root_ / L"Users\\owner\\Documents";
  Win32FileSystem fs_;
  OnePlan plans_;
};

TEST_F(SafeRemoveOnDisk, FolderWithJunctionRemovedTargetIntact) {
  write_file(root() / L"store\\pkg\\index.js", 1000);
  write_file(root() / L"work\\node_modules\\real\\a.js", 1000);
  ASSERT_TRUE(make_junction(root() / L"work\\node_modules\\pkg", root() / L"store\\pkg"));
  ASSERT_TRUE(make_junction(root() / L"work\\node_modules\\docs", documents()));

  const safety::ItemOutcome outcome = remove(add_item(root() / L"work\\node_modules"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::removed) << outcome.reason;
  EXPECT_GT(outcome.freed, Bytes{});
  EXPECT_FALSE(entry_exists(root() / L"work\\node_modules"));
  EXPECT_TRUE(entry_exists(root() / L"store\\pkg\\index.js"));
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

TEST_F(SafeRemoveOnDisk, FolderSwappedForJunctionRefused) {
  write_file(root() / L"work\\target\\obj\\a.o", 10);
  const PlanItemId item = add_item(root() / L"work\\target");
  std::filesystem::remove_all(root() / L"work\\target");
  ASSERT_TRUE(make_junction(root() / L"work\\target", documents()));

  const safety::ItemOutcome outcome = remove(item);
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "changed_since_plan");
  EXPECT_TRUE(entry_exists(root() / L"work\\target"));
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

TEST_F(SafeRemoveOnDisk, ItemBehindAJunctionRefused) {
  write_file(root() / L"data\\file.txt", 10);
  ASSERT_TRUE(make_junction(root() / L"work\\link", root() / L"data"));
  // The identity is the real file's: only the junction in the path gives the detour away.
  const safety::ItemOutcome outcome = remove(add_item(root() / L"work\\link\\file.txt"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "reparse_point_in_path");
  EXPECT_TRUE(entry_exists(root() / L"data\\file.txt"));
}

TEST_F(SafeRemoveOnDisk, OfflineFileSkipped) {
  const path file = root() / L"work\\cloud.bin";
  write_file(file, 10);
  ASSERT_NE(::SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_OFFLINE), FALSE);
  const safety::ItemOutcome outcome = remove(add_item(file));
  EXPECT_EQ(outcome.disposition, safety::Disposition::skipped);
  EXPECT_EQ(outcome.reason, "cloud_placeholder");
  EXPECT_TRUE(entry_exists(file));
}

TEST_F(SafeRemoveOnDisk, DocumentsRefusedInAnySpelling) {
  std::wstring spelled = (documents() / L"thesis.docx").native();
  for (wchar_t& character : spelled) {
    character = static_cast<wchar_t>(::towupper(character));
  }
  const safety::ItemOutcome outcome = remove(add_item(spelled));
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "deny_list:user_known_folder");
  EXPECT_TRUE(entry_exists(documents() / L"thesis.docx"));
}

// A folder moved to a sandbox on another volume (MYCLEAN_TEST_SECOND_VOLUME), as a quarantine on
// a nearly full volume moves it to the overflow volume.
class SafeRemoveAcrossVolumesOnDisk : public SafeRemoveOnDisk {
 protected:
  void SetUp() override {
    const std::optional<path> base = second_volume();
    if (!base) {
      GTEST_SKIP() << "no second volume: set MYCLEAN_TEST_SECOND_VOLUME to a folder on another "
                      "volume than %TEMP% (a sandbox is made and removed inside it)";
    }
    other_root_ = other_.emplace(*base).root();
  }

  [[nodiscard]] path destination(const wchar_t* name) const { return other_root_ / name; }

 private:
  std::optional<Sandbox> other_;
  path other_root_;
};

TEST_F(SafeRemoveAcrossVolumesOnDisk, MovesAFolderCompletely) {
  write_file(root() / L"cache\\zig\\o\\a.bin", 70'000);
  write_file(root() / L"cache\\zig\\o\\deep\\b.bin", 3);
  std::filesystem::create_directories(root() / L"cache\\zig\\empty");
  // Read-only, like a git object file: copied as such, and its source still removed.
  ASSERT_NE(::SetFileAttributesW((root() / L"cache\\zig\\o\\deep\\b.bin").c_str(),
                                 FILE_ATTRIBUTE_READONLY),
            FALSE);
  const safety::ItemOutcome outcome =
      move(add_item(root() / L"cache\\zig"), destination(L"q\\1\\1\\zig"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::moved) << outcome.reason << outcome.detail;
  EXPECT_GE(outcome.freed, Bytes{70'000});
  EXPECT_FALSE(entry_exists(root() / L"cache\\zig"));
  EXPECT_EQ(fs().stat(destination(L"q\\1\\1\\zig\\o\\a.bin"))->size, Bytes{70'000});
  EXPECT_EQ(fs().stat(destination(L"q\\1\\1\\zig\\o\\deep\\b.bin"))->size, Bytes{3});
  EXPECT_EQ(fs().stat(destination(L"q\\1\\1\\zig\\empty"))->kind, os::EntryKind::directory);
  EXPECT_NE(::GetFileAttributesW(destination(L"q\\1\\1\\zig\\o\\deep\\b.bin").c_str()) &
                FILE_ATTRIBUTE_READONLY,
            0U);
}

TEST_F(SafeRemoveAcrossVolumesOnDisk, ALockedFileLeavesTheFolderWhereItWas) {
  write_file(root() / L"cache\\zig\\a.bin", 1000);
  write_file(root() / L"cache\\zig\\z.bin", 1000);
  const wil::unique_hfile writer{::CreateFileW((root() / L"cache\\zig\\z.bin").c_str(),
                                               GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
  ASSERT_TRUE(writer);
  const safety::ItemOutcome outcome =
      move(add_item(root() / L"cache\\zig"), destination(L"q\\zig"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::failed);
  EXPECT_EQ(outcome.reason, "in_use");
  EXPECT_EQ(fs().stat(root() / L"cache\\zig\\a.bin")->size, Bytes{1000});
  EXPECT_TRUE(entry_exists(root() / L"cache\\zig\\z.bin"));
  EXPECT_FALSE(entry_exists(destination(L"q\\zig")));
}

TEST_F(SafeRemoveAcrossVolumesOnDisk, AJunctionInsideIsNotFollowedAndKeepsTheFolder) {
  write_file(root() / L"store\\pkg\\index.js", 1000);
  write_file(root() / L"work\\node_modules\\real\\a.js", 1000);
  ASSERT_TRUE(make_junction(root() / L"work\\node_modules\\pkg", root() / L"store\\pkg"));
  const safety::ItemOutcome outcome =
      move(add_item(root() / L"work\\node_modules"), destination(L"q\\node_modules"));
  EXPECT_EQ(outcome.disposition, safety::Disposition::refused);
  EXPECT_EQ(outcome.reason, "reparse_point_inside");
  EXPECT_TRUE(entry_exists(root() / L"work\\node_modules\\real\\a.js"));
  EXPECT_TRUE(entry_exists(root() / L"store\\pkg\\index.js"));
  EXPECT_FALSE(entry_exists(destination(L"q\\node_modules")));
}

// Throughput of a quarantine of many small files to another volume: the folder copy with one
// flush (what SafeRemove does now) against MoveFileEx with MOVEFILE_WRITE_THROUGH per file (what
// moving the files one by one cost). Printed, not asserted: it depends on the drives.
TEST_F(SafeRemoveAcrossVolumesOnDisk, MeasuresTheFolderCopyAgainstAWriteThroughMovePerFile) {
  constexpr int files = 1500;
  constexpr std::size_t file_size = std::size_t{16} * 1024;
  const auto fill = [&](const path& folder) {
    for (int index = 0; index < files; ++index) {
      write_file(folder / std::to_wstring(index % 30) / (std::to_wstring(index) + L".o"),
                 file_size);
    }
  };
  const double megabytes = files * static_cast<double>(file_size) / 1e6;
  const auto seconds_since = [](std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };

  // The last file copied ends the copy; the flush and the removal of the source follow.
  struct CopyClock final : safety::IProgress {
    CopyClock() = default;
    void begin(std::size_t /*items*/, Bytes /*bytes*/) override {}
    void item_started(const PlanItem& /*item*/) override {}
    void file_done(Bytes /*bytes*/) override { copied = std::chrono::steady_clock::now(); }
    void item_finished(const safety::ItemOutcome& /*outcome*/) override {}
    std::chrono::steady_clock::time_point copied;
  };
  fill(root() / L"a");
  CopyClock clock;
  const std::vector<safety::MoveRequest> requests = {
      {.item = add_item(root() / L"a"), .destination = destination(L"a")}};
  auto start = std::chrono::steady_clock::now();
  const safety::ItemOutcome outcome = remover().move(PlanId{1}, requests, &clock).front();
  const double folder_seconds = seconds_since(start);
  const double copy_seconds = std::chrono::duration<double>(clock.copied - start).count();
  ASSERT_EQ(outcome.disposition, safety::Disposition::moved) << outcome.reason << outcome.detail;

  fill(root() / L"b");
  start = std::chrono::steady_clock::now();
  for (int index = 0; index < files; ++index) {
    const path relative = path(std::to_wstring(index % 30)) / (std::to_wstring(index) + L".o");
    std::filesystem::create_directories(destination(L"b") / relative.parent_path());
    ASSERT_TRUE(fs().move_file(root() / L"b" / relative, destination(L"b") / relative));
  }
  const double per_file_seconds = seconds_since(start);

  std::cout << "  " << files << " files, " << megabytes << " MB to " << destination(L"").string()
            << "\n  folder copy, one flush: " << folder_seconds << " s, "
            << megabytes / folder_seconds << " MB/s (copy " << copy_seconds << " s, flush and "
            << "removal " << folder_seconds - copy_seconds
            << " s)\n  MoveFileEx write-through per file: " << per_file_seconds << " s, "
            << megabytes / per_file_seconds << " MB/s\n";
  RecordProperty("folder_copy_mb_per_s", std::to_string(megabytes / folder_seconds));
  RecordProperty("write_through_mb_per_s", std::to_string(megabytes / per_file_seconds));
}

}  // namespace
}  // namespace myclean::platform
