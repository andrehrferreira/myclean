#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/cleanup/command_runner.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

namespace myclean::cleanup {

// How a tool's locate command reports its cache.
enum class CacheReport {
  paths,    // absolute paths, one per line, optionally after "<label>: "
  docker,   // `docker system df` lines "<type>=<reclaimable> (<percent>)"
  wsl_tmp,  // `wsl -l -q`: one WSL distribution per line; prune runs inside each one
};

// A package manager's own prune command, as allowed for rules with strategy "native"
// (docs/analysis/myclean-planning/02-disk-cleaning-and-relocation.md, F-013). Arguments are
// space separated; none contains a space.
struct NativeTool {
  std::string_view command;  // as a rule declares it: "pnpm store prune"
  std::string_view program;  // resolved on PATH by the runner
  std::string_view prune;    // the arguments apply() runs: non-interactive
  std::string_view locate;   // read-only arguments that report the cache
  CacheReport report = CacheReport::paths;
};

// The allowed tool whose `command` equals `command` (runs of spaces count as one); nullptr when
// the command is not allowed. The catalog loader uses it to reject unknown commands.
[[nodiscard]] const NativeTool* find_native_tool(std::string_view command);

// Prunes a global package cache with the tool's own command, run in the tray under the owner's
// token and never as a service account (cleaning spec, "Native Prune Commands Under the User
// Token"). MyClean deletes nothing itself. Not reversible: the tool downloads what it needs
// again. Every reference must outlive the action.
class NativeCommand final : public Action {
 public:
  static constexpr std::chrono::milliseconds locate_timeout{std::chrono::minutes(2)};
  static constexpr std::chrono::milliseconds prune_timeout{std::chrono::minutes(30)};
  // A WSL /tmp can hold tens of thousands of folders and ~100 GB (seen on the reference
  // machine), which `du` and `rm` inside WSL 1 take far longer over than a package cache.
  static constexpr std::chrono::milliseconds wsl_measure_timeout{std::chrono::minutes(20)};
  static constexpr std::chrono::milliseconds wsl_prune_timeout{std::chrono::hours(4)};

  NativeCommand(ICommandRunner& runner, const os::IFileSystem& file_system, std::string command,
                Tier tier, std::stop_token stop = {});

  [[nodiscard]] ActionInfo describe() const override;
  // Asks the tool read-only where its cache is: one item per cache folder with its size on
  // disk (docker: one per reclaimable type, "docker:<type>"; wsl: one per distribution but
  // Docker Desktop's, "wsl:<distribution>:/tmp", sized by `du` inside it, a distribution whose
  // `du` fails left out unless all fail). Fails with refused when the command is not allowed or
  // the runner has no user context, and with io_error when the tool fails.
  [[nodiscard]] Result<Preview> preview() override;
  // Runs the prune command once, unless the preview is empty (wsl: once inside each previewed
  // distribution, all of them even when one fails). A non-zero exit code fails with io_error
  // and the tool's first error line. Undo lists the preview items (data: the path);
  // not_reversible is "regenerates".
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "cache_bytes": the applied preview's total against a new preview.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Always fails with not_reversible: "not reversible: regenerates".
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  [[nodiscard]] Result<const NativeTool*> allowed_tool() const;
  [[nodiscard]] Result<CommandOutput> run(const NativeTool& tool, std::string_view arguments,
                                          std::chrono::milliseconds timeout) const;
  [[nodiscard]] Result<Preview> wsl_preview(const NativeTool& tool, std::string_view listed) const;
  [[nodiscard]] Result<void> wsl_prune(const NativeTool& tool, const Preview& preview) const;

  ICommandRunner* runner_;
  const os::IFileSystem* file_system_;
  std::string command_;
  Tier tier_;
  std::stop_token stop_;
  Bytes before_;
};

}  // namespace myclean::cleanup
