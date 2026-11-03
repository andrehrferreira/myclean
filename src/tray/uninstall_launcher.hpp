#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string_view>

#include "core/model/result.hpp"
#include "core/user_tasks/uninstall.hpp"

namespace myclean::tray {

// user_tasks::IUninstallLauncher over ShellExecuteExW ("open", no error message boxes): the
// uninstaller runs under the tray's token (the owner's) in a normal window, and Windows shows
// the UAC prompt when it needs elevation. A prompt the owner declines fails with refused. `show`
// false starts the program hidden; only the tests use it, and they never start an uninstaller.
class Win32UninstallLauncher final : public user_tasks::IUninstallLauncher {
 public:
  explicit Win32UninstallLauncher(bool show = true) : show_(show) {}

  [[nodiscard]] Result<void> launch(const std::filesystem::path& program,
                                    std::string_view arguments, std::chrono::minutes wait,
                                    const std::stop_token& stop) override;

 private:
  bool show_;
};

}  // namespace myclean::tray
