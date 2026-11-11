#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/space/scanner.hpp"

// The repository safety check (backup spec, "Repository Safety Check"): the git repositories
// under the workspace roots with work that exists nowhere else. Read-only and local: no fetch, no
// push, no commit, nothing that writes to a repository.
namespace myclean::backup {

// What git says about one work tree, from local data only.
struct RepoState {
  std::size_t remotes = 0;     // `git remote`
  std::uint64_t unpushed = 0;  // commits of local branches on no remote-tracking ref
  bool uncommitted = false;    // `git status --porcelain` lists anything
};

// Read-only questions to git about a work tree. Implementations never fetch, never write to the
// repository and never start a background process (src/platform/git: GitCli).
class IRepoGit {
 public:
  virtual ~IRepoGit() = default;
  IRepoGit(const IRepoGit&) = delete;
  IRepoGit& operator=(const IRepoGit&) = delete;
  IRepoGit(IRepoGit&&) = delete;
  IRepoGit& operator=(IRepoGit&&) = delete;

  [[nodiscard]] virtual Result<RepoState> state(const std::filesystem::path& work_tree) const = 0;

 protected:
  IRepoGit() = default;
};

struct RepoLimits {
  int max_depth = 8;                  // folder levels below a root
  std::size_t max_folders = 200'000;  // folders listed per root
};

// The work trees under `roots`: folders holding a `.git` entry (a folder, or the file of a
// linked worktree or submodule). A work tree is not entered further, a reparse point is never
// entered, and folders named in `skip` (any case) are not entered. Roots that do not exist are
// left out.
struct RepoWalk {
  std::vector<std::filesystem::path> repositories;
  bool truncated = false;  // a root stopped at limits.max_folders
};
[[nodiscard]] RepoWalk find_repositories(const space::IDirectoryReader& reader,
                                         std::span<const std::filesystem::path> roots,
                                         std::span<const std::wstring> skip,
                                         const RepoLimits& limits, const std::stop_token& stop);

// One repository with unprotected work.
struct RepoFinding {
  std::filesystem::path path;
  bool no_remote = false;
  std::uint64_t unpushed = 0;
  bool uncommitted = false;
  std::string text;  // "no remote", "4 unpushed commits", "uncommitted changes", joined by "; "
};

struct RepoReport {
  std::size_t checked = 0;               // repositories git described
  std::vector<RepoFinding> unprotected;  // in path order
  std::vector<std::string> problems;     // repositories git could not describe, with the error
  bool truncated = false;
};

// Asks `git` about each repository of `repositories` and keeps those with no remote, unpushed
// commits or uncommitted changes.
[[nodiscard]] RepoReport check_repositories(const IRepoGit& git,
                                            std::span<const std::filesystem::path> repositories,
                                            const std::stop_token& stop);

}  // namespace myclean::backup
