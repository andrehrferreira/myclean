#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/packs/applies.hpp"

// What knowledge packs need from Windows: the machine their applies_when asks about, and the
// owner's override folder, accepted only when administrators alone can write to it.
namespace myclean::platform {

// packs::IMachine over this machine. Read-only.
class Win32Machine final : public packs::IMachine {
 public:
  // `user_sid`: the user whose programs (Uninstall key of HKCU) and Appx packages count, read
  // from HKEY_USERS (the service, for the owner); empty: the caller's own HKEY_CURRENT_USER.
  explicit Win32Machine(std::wstring user_sid = {}) : user_sid_(std::move(user_sid)) {}

  // DisplayName of every Uninstall subkey of HKLM (64- and 32-bit views) and of the user, and
  // the full name of every Appx package registered for the user
  // ("PythonSoftwareFoundation.Python.3.12_3.12.2800.0_x64__qbz5n2kfra8p0"). A key that cannot
  // be read adds nothing.
  [[nodiscard]] std::vector<std::string> installed_apps() const override;
  // From a process snapshot; empty when it cannot be taken.
  [[nodiscard]] std::vector<std::string> running_images() const override;
  // CurrentBuildNumber of HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion; 0 when unreadable.
  [[nodiscard]] std::uint32_t os_build() const override;

 private:
  std::wstring user_sid_;
};

// Fails with refused, naming the trustee, unless the security descriptor `sddl` (SDDL form) is
// owned by SYSTEM, Administrators or TrustedInstaller and its DACL lets no one else write
// (write data, append, write attributes or extended attributes, delete, delete children, change
// permissions or owner, generic write or all). Inherit-only ACEs and CREATOR OWNER do not apply
// to the object itself and are not counted; deny ACEs are not counted either.
[[nodiscard]] Result<void> check_admin_write_only(std::wstring_view sddl);

// %ProgramData%\MyClean\packs: the owner's override packs.
[[nodiscard]] Result<std::filesystem::path> owner_packs_dir();

// `folder` when it exists and it and everything in it pass check_admin_write_only (and none is a
// reparse point); nullopt when it does not exist. Fails with refused otherwise: such a folder is
// ignored, since anyone able to write there could make the service clean or move anything.
[[nodiscard]] Result<std::optional<std::filesystem::path>> owner_packs_folder(
    const std::filesystem::path& folder);

}  // namespace myclean::platform
