#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

struct ProcessOutput {
  std::uint32_t exit_code = 0;
  std::string out;  // everything written to standard output
};

// Runs `program` with `arguments` without a window (CREATE_NO_WINDOW), standard input and error
// on NUL and standard output captured, inheriting no handle but those. The process and every
// process it starts run in a job that is killed after `timeout` (then the call fails with
// io_error) or when the call returns.
[[nodiscard]] Result<ProcessOutput> run_hidden(const std::filesystem::path& program,
                                               std::span<const std::wstring> arguments,
                                               std::chrono::milliseconds timeout);

// The first git.exe in the folders of PATH; nullopt when there is none.
[[nodiscard]] std::optional<std::filesystem::path> find_git();

// IGit through the git command line. Every command is read-only and runs as
//   git --no-optional-locks -c core.fsmonitor=false -C <work tree> ...
// so it takes no index lock and starts no fsmonitor daemon. Commands: rev-parse -q --verify
// HEAD, log -1 --format=%ct HEAD, check-ignore -q -- <path>, ls-files -z -- :(literal)<path>.
// git's own safety checks apply: a repository git refuses (such as one owned by another user)
// fails with io_error, and the engine then proposes nothing from it.
class GitCli final : public clean::IGit {
 public:
  explicit GitCli(std::filesystem::path git,
                  std::chrono::milliseconds timeout = std::chrono::seconds{60});

  [[nodiscard]] Result<std::optional<WallTime>> last_commit(
      const std::filesystem::path& work_tree) const override;
  [[nodiscard]] Result<bool> ignored(const std::filesystem::path& work_tree,
                                     const std::filesystem::path& entry) const override;
  [[nodiscard]] Result<bool> tracks_files(const std::filesystem::path& work_tree,
                                          const std::filesystem::path& entry) const override;

 private:
  [[nodiscard]] Result<ProcessOutput> run(const std::filesystem::path& work_tree,
                                          std::vector<std::wstring> arguments) const;

  std::filesystem::path git_;
  std::chrono::milliseconds timeout_;
};

}  // namespace myclean::platform
