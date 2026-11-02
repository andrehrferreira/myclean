#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/units.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"

// Merged idle worktrees (scan-all spec, "Merged Idle Worktrees"): linked git worktrees whose
// HEAD is in the default branch, with nothing to commit and nothing changed for weeks. Read-only:
// git is only asked.
namespace myclean::scanall {

struct WorktreeOptions {
  Bytes min_size{std::uint64_t{64} << 20U};  // smaller worktrees are not proposed
  std::chrono::days idle{14};                // nothing inside changed for this long
};

struct WorktreeRun {
  std::vector<Finding> findings;
  std::size_t worktrees = 0;  // linked worktrees git described
  std::size_t errors = 0;     // folders git could not answer for
};

// The linked worktrees of `tree` (the drive `drive`): folders holding a `.git` FILE, of at least
// min_size, completely listed and unchanged (folder_times) for `idle`, that git describes as a
// linked worktree whose HEAD (a branch or detached) is the repository's default branch or an
// ancestor of it and whose status lists nothing. Each is a remove finding with a
// remove_worktree plan naming the worktree and its repository, sized by the folder. A `.git`
// folder is never entered, and a folder that holds its repository's git folder is never
// proposed.
[[nodiscard]] WorktreeRun find_worktrees(const space::ScanResult& tree, const FolderTimes& times,
                                         const DriveInfo& drive, const clean::IWorktreeGit& git,
                                         WallTime now, const WorktreeOptions& options = {});

}  // namespace myclean::scanall
