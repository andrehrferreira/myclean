#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <wil/resource.h>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

struct ProcessOutput {
  std::uint32_t exit_code = 0;
  std::string out;  // everything written to standard output
};

// A user to run a process as: a primary token of that user's logon session and the user's
// environment block (CreateEnvironmentBlock: NUL-terminated entries, then one more NUL).
struct ProcessIdentity {
  wil::unique_handle token;
  std::wstring environment;
};
// The identity of each command, read again for every one; empty: this process's own.
using IdentitySource = std::function<Result<ProcessIdentity>()>;

// Runs `program` with `arguments` without a window (CREATE_NO_WINDOW), standard input and error
// on NUL and standard output captured, inheriting no handle but those. The process and every
// process it starts run in a job that is killed after `timeout` (then the call fails with
// io_error) or when the call returns. With `identity` it runs as that user, on the user's
// desktop (CreateProcessAsUser; needs the privileges of LocalSystem unless the token is a
// restricted copy of this process's own).
[[nodiscard]] Result<ProcessOutput> run_hidden(const std::filesystem::path& program,
                                               std::span<const std::wstring> arguments,
                                               std::chrono::milliseconds timeout,
                                               const ProcessIdentity* identity = nullptr);

// The first git.exe in the folders of PATH; nullopt when there is none.
[[nodiscard]] std::optional<std::filesystem::path> find_git();

// IGit and IWorktreeGit through the git command line. Every command is read-only and runs as
//   git --no-optional-locks -c core.fsmonitor=false -C <work tree> ...
// so it takes no index lock and starts no fsmonitor daemon. Commands: rev-parse -q --verify
// HEAD, log -1 --format=%ct HEAD, check-ignore -q -- <path>, ls-files -z -- :(literal)<path>;
// for worktrees rev-parse --path-format=absolute --git-dir --git-common-dir, symbolic-ref -q
// --short HEAD and refs/remotes/origin/HEAD, rev-parse -q --verify refs/heads/main (master),
// merge-base --is-ancestor HEAD <base> and status --porcelain.
// git's own safety checks apply: a repository git refuses (such as one owned by another user)
// fails with io_error, and the engine then proposes nothing from it. So the service runs git as
// the owner (`identity`); a command whose identity cannot be read fails the same way.
class GitCli final : public clean::IGit, public clean::IWorktreeGit {
 public:
  explicit GitCli(std::filesystem::path git, IdentitySource identity = {},
                  std::chrono::milliseconds timeout = std::chrono::seconds{60});

  [[nodiscard]] Result<std::optional<WallTime>> last_commit(
      const std::filesystem::path& work_tree) const override;
  [[nodiscard]] Result<bool> ignored(const std::filesystem::path& work_tree,
                                     const std::filesystem::path& entry) const override;
  [[nodiscard]] Result<bool> tracks_files(const std::filesystem::path& work_tree,
                                          const std::filesystem::path& entry) const override;
  [[nodiscard]] Result<std::optional<clean::Worktree>> worktree(
      const std::filesystem::path& folder) const override;

 private:
  [[nodiscard]] Result<ProcessOutput> run(const std::filesystem::path& work_tree,
                                          std::vector<std::wstring> arguments) const;
  // The short name `ref` points to; nullopt when it is not a symbolic ref.
  [[nodiscard]] Result<std::optional<std::string>> symbolic_ref(
      const std::filesystem::path& work_tree, const std::wstring& ref) const;
  // origin/HEAD's branch, else main, else master; empty when there is none.
  [[nodiscard]] Result<std::string> default_branch(const std::filesystem::path& work_tree) const;
  // Whether HEAD is `base` or an ancestor of it, and then whether nothing is to commit.
  [[nodiscard]] Result<void> merged_and_clean(const std::filesystem::path& work_tree,
                                              clean::Worktree& worktree) const;

  std::filesystem::path git_;
  IdentitySource identity_;
  std::chrono::milliseconds timeout_;
};

}  // namespace myclean::platform
