#include "core/scanall/worktrees.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/cold.hpp"
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
using testing::now;

const WallTime idle = std::chrono::sys_days{std::chrono::year{2026} / 9 / 1};
const WallTime recent = std::chrono::sys_days{std::chrono::year{2026} / 10 / 25};
const fs::path repository = LR"(E:\Repo\.git)";

// Answers by folder; a folder it has no answer for is not a linked worktree.
class FakeWorktreeGit final : public clean::IWorktreeGit {
 public:
  std::map<fs::path, Result<std::optional<clean::Worktree>>> answers;
  mutable std::vector<fs::path> asked;

  [[nodiscard]] Result<std::optional<clean::Worktree>> worktree(
      const fs::path& folder) const override {
    asked.push_back(folder);
    const auto found = answers.find(folder);
    if (found == answers.end()) {
      return std::optional<clean::Worktree>{};
    }
    return found->second;
  }
};

clean::Worktree linked(std::string branch, bool merged = true, bool clean = true) {
  return clean::Worktree{.repository = repository,
                         .branch = std::move(branch),
                         .base = "origin/main",
                         .merged = merged,
                         .clean = clean};
}

// E: with a repository, its worktrees under .claude\worktrees and a submodule. Each work tree
// holds a `.git` (file or folder) and 8 KiB of content written at `time`.
class Worktrees : public ::testing::Test {
 protected:
  void add(const fs::path& folder, bool git_file, WallTime time) {
    std::vector<space::ListedEntry>& entries = reader_.listings[folder];
    entries.push_back(git_file ? testing::file(".git", 1_KiB, time, next_id_++)
                               : testing::folder(".git"));
    entries.push_back(testing::file("data.bin", 8_KiB, time, next_id_++));
    for (fs::path path = folder; path.has_relative_path(); path = path.parent_path()) {
      std::vector<space::ListedEntry>& parent = reader_.listings[path.parent_path()];
      const std::string leaf = path.filename().string();
      if (std::ranges::find(parent, fs::path(leaf), &space::ListedEntry::name) == parent.end()) {
        parent.push_back(testing::folder(leaf));
      }
    }
  }

  [[nodiscard]] space::ScanResult tree() { return scan_ok(reader_, LR"(E:\)"); }
  [[nodiscard]] FakeDirectoryReader& reader() { return reader_; }

 private:
  FakeDirectoryReader reader_;
  std::uint8_t next_id_ = 1;
};

TEST_F(Worktrees, ProposesMergedCleanIdleWorktrees) {
  add(LR"(E:\Repo)", false, idle);
  reader().listings[LR"(E:\Repo\.git)"] = {testing::file("HEAD", 1_KiB, idle, 200)};
  add(LR"(E:\Repo\.claude\worktrees\agent-1)", true, idle);
  add(LR"(E:\Repo\.claude\worktrees\agent-2)", true, idle);
  const space::ScanResult scanned = tree();
  const scanall::FolderTimes times = folder_times(scanned);
  FakeWorktreeGit git;
  git.answers[LR"(E:\Repo\.claude\worktrees\agent-1)"] = linked("agent-1");
  git.answers[LR"(E:\Repo\.claude\worktrees\agent-2)"] = linked("");  // detached

  const WorktreeRun run = find_worktrees(scanned, times, drive(LR"(E:\)", DriveClass::nvme), git,
                                         now, {.min_size = 4_KiB});

  EXPECT_EQ(run.worktrees, 2U);
  EXPECT_EQ(run.errors, 0U);
  ASSERT_EQ(run.findings.size(), 2U);
  const Finding& finding = run.findings[0];
  EXPECT_EQ(finding.group, ActionGroup::remove);
  EXPECT_EQ(finding.source, "worktrees");
  EXPECT_EQ(finding.drive, fs::path(LR"(E:\)"));
  EXPECT_EQ(finding.path, fs::path(LR"(E:\Repo\.claude\worktrees\agent-1)"));
  EXPECT_EQ(finding.bytes, 9_KiB);
  ASSERT_EQ(finding.items.size(), 1U);
  EXPECT_EQ(finding.plan.kind, PlanKind::remove_worktree);
  EXPECT_EQ(to_string(finding.plan.kind), "remove_worktree");
  EXPECT_EQ(*finding.plan.param("worktree"), R"(E:\Repo\.claude\worktrees\agent-1)");
  EXPECT_EQ(*finding.plan.param("repository"), R"(E:\Repo\.git)");
  EXPECT_NE(finding.reason.find("branch agent-1 is merged into origin/main"), std::string::npos)
      << finding.reason;
  EXPECT_NE(finding.reason.find("since 2026-09-01"), std::string::npos) << finding.reason;
  EXPECT_NE(finding.reason.find("ignores (build output, .env) go with the folder"),
            std::string::npos)
      << finding.reason;
  EXPECT_NE(run.findings[1].reason.find("whose detached HEAD is merged"), std::string::npos)
      << run.findings[1].reason;
}

TEST_F(Worktrees, KeepsUnmergedDirtyRecentSmallAndOtherWorkTrees) {
  add(LR"(E:\Repo)", false, idle);
  add(LR"(E:\Repo\.claude\worktrees\unmerged)", true, idle);
  add(LR"(E:\Repo\.claude\worktrees\dirty)", true, idle);
  add(LR"(E:\Repo\.claude\worktrees\recent)", true, recent);
  add(LR"(E:\Repo\.claude\worktrees\broken)", true, idle);
  add(LR"(E:\Repo\sub)", true, idle);  // a submodule: git says it is not a linked worktree
  // Never entered: a repository's git folder.
  add(LR"(E:\Repo\.git\modules\odd)", true, idle);
  // A folder that holds its repository is never proposed.
  add(LR"(E:\Outer)", true, idle);
  const space::ScanResult scanned = tree();
  const scanall::FolderTimes times = folder_times(scanned);
  FakeWorktreeGit git;
  git.answers[LR"(E:\Repo\.claude\worktrees\unmerged)"] = linked("feature", false, false);
  git.answers[LR"(E:\Repo\.claude\worktrees\dirty)"] = linked("fix", true, false);
  git.answers[LR"(E:\Repo\.claude\worktrees\recent)"] = linked("new");
  git.answers[LR"(E:\Repo\.claude\worktrees\broken)"] = fail(Errc::io_error, "git refused");
  clean::Worktree outer = linked("main");
  outer.repository = LR"(E:\Outer\inner\.git)";
  git.answers[LR"(E:\Outer)"] = outer;

  const WorktreeRun run = find_worktrees(scanned, times, drive(LR"(E:\)", DriveClass::nvme), git,
                                         now, {.min_size = 4_KiB});

  EXPECT_TRUE(run.findings.empty());
  EXPECT_EQ(run.worktrees, 3U);  // unmerged, dirty, outer
  EXPECT_EQ(run.errors, 1U);
  const auto asked = [&git](const wchar_t* path) {
    return std::ranges::find(git.asked, fs::path(path)) != git.asked.end();
  };
  EXPECT_FALSE(asked(LR"(E:\Repo)"));
  EXPECT_FALSE(asked(LR"(E:\Repo\.claude\worktrees\recent)"));
  EXPECT_FALSE(asked(LR"(E:\Repo\.git\modules\odd)"));
  EXPECT_TRUE(asked(LR"(E:\Repo\sub)"));

  const WorktreeRun large = find_worktrees(scanned, times, drive(LR"(E:\)", DriveClass::nvme), git,
                                           now, {.min_size = 1_MiB});
  EXPECT_EQ(large.worktrees + large.errors, 0U);
}

}  // namespace
}  // namespace myclean::scanall
