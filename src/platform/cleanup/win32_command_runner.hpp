#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>
#include <vector>

#include "core/cleanup/command_runner.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// Runs commands as hidden child processes with this process's token: in the tray that is the
// owner's token, in the service LocalSystem. Each child runs in a job object, so a timeout or a
// cancel ends its whole tree, and any process it leaves behind is ended when it exits. Standard
// input is NUL, so a tool that asks a question reads end of file. Every child starts in the
// profile folder of this process's user (%USERPROFILE%), never in this process's own folder.
class Win32CommandRunner final : public cleanup::ICommandRunner {
 public:
  // `search_path`: the folders searched for a bare program name, in order; empty means the
  // PATH of this process. The current folder is never searched.
  explicit Win32CommandRunner(std::vector<std::filesystem::path> search_path = {});

  [[nodiscard]] bool user_context() const override;
  [[nodiscard]] Result<cleanup::CommandOutput> run(const cleanup::Command& command,
                                                   std::stop_token stop) override;

  // The absolute path of `program`: itself when absolute, else the first existing
  // `<folder>\<program><ext>` of the search path with ext .com, .exe, .bat or .cmd (or none,
  // when `program` already ends in one). A .bat or .cmd runs through cmd.exe, which refuses
  // arguments holding a cmd.exe metacharacter.
  [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view program) const;

 private:
  std::vector<std::filesystem::path> search_path_;
};

// %SystemRoot%\System32\dism.exe.
[[nodiscard]] std::filesystem::path dism_path();

}  // namespace myclean::platform
