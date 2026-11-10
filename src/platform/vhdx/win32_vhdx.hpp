#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

#include "core/model/result.hpp"
#include "core/vhdx/compact.hpp"
#include "platform/git/git_cli.hpp"

namespace myclean::platform {

// Compacts a detached .vhdx with the Virtual Disk API, which diskpart's `compact vdisk` and
// Optimize-VHD also use, so neither Hyper-V nor a script is needed: OpenVirtualDisk
// (OPEN_VIRTUAL_DISK_VERSION_2, VIRTUAL_DISK_ACCESS_NONE, as a VHDX requires) and
// CompactVirtualDisk without flags (a file-system-agnostic compaction of the detached disk,
// waiting for its end). A disk attached by WSL or Docker Desktop fails to open with a sharing
// violation: in_use.
class Win32VirtualDisks final : public vhdx::IVirtualDisks {
 public:
  Win32VirtualDisks() = default;

  [[nodiscard]] Result<void> compact(const std::filesystem::path& file) override;
};

// wsl.exe of this machine: %ProgramW6432%\WSL\wsl.exe (the WSL package), else
// %SystemRoot%\System32\wsl.exe (the inbox launcher); nullopt when neither exists.
[[nodiscard]] std::optional<std::filesystem::path> wsl_path();

// Runs `wsl.exe --shutdown` (wsl_path(); the only arguments) hidden, as the user `identity`
// gives (empty: this process's own user), through run_hidden: WSL keeps a virtual machine per
// user, so the service shuts the owner's down as the owner. Fails with not_found when WSL is not
// installed and with io_error for a non-zero exit code.
class Win32Wsl final : public vhdx::IWsl {
 public:
  explicit Win32Wsl(IdentitySource identity = {},
                    std::chrono::milliseconds timeout = std::chrono::minutes{2})
      : identity_(std::move(identity)), timeout_(timeout) {}

  [[nodiscard]] Result<void> shutdown() override;

 private:
  IdentitySource identity_;
  std::chrono::milliseconds timeout_;
};

}  // namespace myclean::platform
