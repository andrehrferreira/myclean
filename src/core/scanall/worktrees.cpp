#include "core/scanall/worktrees.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;

bool git_name(const fs::path& name) {
  return space::lower_ascii(space::utf8(name)) == ".git";
}

// A linked worktree, or a submodule, holds a `.git` file; a repository's own work tree holds
// a `.git` folder.
bool holds_git_file(const space::Folder& folder) {
  return std::ranges::any_of(folder.files, [](const space::FileItem& file) {
    return !file.link && !file.placeholder && git_name(file.name);
  });
}

Finding worktree_finding(const space::Folder& folder, WallTime newest,
                         const clean::Worktree& worktree, const DriveInfo& drive) {
  const Bytes size = folder.size_on_disk;
  return Finding{
      .group = ActionGroup::remove,
      .source = "worktrees",
      .drive = drive.root,
      .path = folder.path,
      .items = {Item{.path = folder.path, .size = size}},
      .bytes = size,
      .confidence = 0.8,
      .reason = std::format(
          "a git worktree of {} whose {} is merged into {}, with nothing to commit and nothing "
          "changed since {:%Y-%m-%d}; `git worktree remove` keeps every commit in the "
          "repository, and the files git ignores (build output, .env) go with the folder",
          space::utf8(worktree.repository),
          worktree.branch.empty() ? std::string("detached HEAD") : "branch " + worktree.branch,
          worktree.base, std::chrono::floor<std::chrono::days>(newest)),
      .plan = Plan{.kind = PlanKind::remove_worktree,
                   .params = {{"worktree", space::utf8(folder.path)},
                              {"repository", space::utf8(worktree.repository)}}}};
}

}  // namespace

WorktreeRun find_worktrees(const space::ScanResult& tree, const FolderTimes& times,
                           const DriveInfo& drive, const clean::IWorktreeGit& git, WallTime now,
                           const WorktreeOptions& options) {
  WorktreeRun run;
  const WallTime cutoff = now - std::chrono::duration_cast<WallTime::duration>(options.idle);
  // Closed: a `.git` folder or a proposed worktree, and everything below it.
  std::vector<bool> closed(tree.folders.size(), false);
  for (std::size_t index = 1; index < tree.folders.size(); ++index) {
    const space::Folder& folder = tree.folders[index];
    closed[index] = closed[folder.parent] || git_name(folder.path.filename());
    if (closed[index] || folder.size_on_disk < options.min_size || !holds_git_file(folder) ||
        !times.complete[index] || times.newest[index] >= cutoff) {
      continue;
    }
    const Result<std::optional<clean::Worktree>> worktree = git.worktree(folder.path);
    if (!worktree) {
      ++run.errors;
      continue;
    }
    if (!*worktree) {
      continue;
    }
    ++run.worktrees;
    const clean::Worktree& linked = **worktree;
    if (linked.merged && linked.clean && !clean::within(linked.repository, folder.path)) {
      run.findings.push_back(worktree_finding(folder, times.newest[index], linked, drive));
      closed[index] = true;
    }
  }
  return run;
}

}  // namespace myclean::scanall
