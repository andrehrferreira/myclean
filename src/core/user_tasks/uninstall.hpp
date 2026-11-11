#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/settings.hpp"

// Launch the uninstaller (owner-decisions spec): the tray runs an installed program's own
// UninstallString with the owner's token when the owner clicks. It is the program's interactive
// uninstaller, shown on the owner's desktop (with the UAC prompt when it needs elevation): this is
// the owner's explicit action.
namespace myclean::user_tasks {

// How long the tray waits for a launched uninstaller to end before it reports the launch.
inline constexpr std::chrono::minutes uninstall_wait{15};

// Starts uninstallers. Implemented in src/tray; tests use a fake that starts nothing.
class IUninstallLauncher {
 public:
  virtual ~IUninstallLauncher() = default;
  IUninstallLauncher(const IUninstallLauncher&) = delete;
  IUninstallLauncher& operator=(const IUninstallLauncher&) = delete;
  IUninstallLauncher(IUninstallLauncher&&) = delete;
  IUninstallLauncher& operator=(IUninstallLauncher&&) = delete;

  // Starts `program` with `arguments` as the caller, in a normal window, then waits up to `wait`
  // for that process to end, returning early when `stop` is requested. Fails when it could not
  // be started (refused when the owner declined the UAC prompt).
  [[nodiscard]] virtual Result<void> launch(const std::filesystem::path& program,
                                            std::string_view arguments, std::chrono::minutes wait,
                                            const std::stop_token& stop) = 0;

 protected:
  IUninstallLauncher() = default;
};

// An UninstallString split into the program and its arguments.
struct CommandLine {
  std::filesystem::path program;
  std::string arguments;

  friend bool operator==(const CommandLine&, const CommandLine&) = default;
};

// `text` split: a quoted first token is the program; otherwise the text up to the first ".exe"
// that ends a word (installers write unquoted paths with spaces: C:\Program Files\X\unins.exe
// /S), else up to the first space. Fails with invalid_argument on empty text.
[[nodiscard]] Result<CommandLine> split_command(std::string_view text);

// Uninstalls one installed program by its own uninstaller (tier ask, run in the owner's
// context). `key` is its Uninstall entry (scanall::uninstall_roots), `name` its DisplayName.
//   preview  one item "uninstall:<name>" with the entry's EstimatedSize; fails with not_found
//            when the entry is gone (uninstalled meanwhile);
//   apply    reads the entry again and refuses when it is gone, names another program or has no
//            UninstallString; starts that UninstallString (split_command) through `launcher` and
//            waits up to uninstall_wait for it. The undo data records what was launched; it
//            cannot be undone (reinstalling needs the program's installer);
//   verify   metric "uninstall_entry": better when the entry is gone; while it is still there it
//            fails with "launched ...; finish in the uninstaller", so the plan stays applied and
//            its log says what was launched. When the entry is gone but its InstallLocation is
//            still there (with `files`), it fails naming that folder: MyClean reports a leftover
//            folder and never deletes it;
//   undo     fails with not_reversible.
class UninstallAction final : public Action {
 public:
  // Every reference must outlive the action.
  UninstallAction(const placement::IRegistry& registry, IUninstallLauncher& launcher,
                  std::string key, std::string name, std::stop_token stop,
                  const os::IFileSystem* files = nullptr)
      : registry_(&registry),
        launcher_(&launcher),
        files_(files),
        key_(std::move(key)),
        name_(std::move(name)),
        stop_(std::move(stop)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  const placement::IRegistry* registry_;
  IUninstallLauncher* launcher_;
  const os::IFileSystem* files_;  // may be null: no leftover folder check
  std::string key_;
  std::string name_;
  std::stop_token stop_;
  std::string launched_;  // the UninstallString apply started
};

}  // namespace myclean::user_tasks
