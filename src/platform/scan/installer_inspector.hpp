#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/space/installers.hpp"

namespace myclean::platform {

// space::IInstallerInspector over the Windows Installer API (MsiOpenDatabase read-only, the
// Property table, MsiQueryProductState), the version resource of executables
// (GetFileVersionInfoEx, loaded as data: no code runs) and the Uninstall registry keys (HKLM
// 64-bit and 32-bit views, HKCU), all read-only. HKCU is the calling account's.
class Win32InstallerInspector final : public space::IInstallerInspector {
 public:
  Win32InstallerInspector() = default;

  [[nodiscard]] Result<std::optional<std::string>> msi_product_code(
      const std::filesystem::path& package) const override;
  [[nodiscard]] Result<std::optional<std::string>> installed_msi_product(
      const std::string& product_code) const override;
  [[nodiscard]] Result<std::optional<space::VersionInfo>> exe_version(
      const std::filesystem::path& executable) const override;
  [[nodiscard]] Result<std::vector<space::InstalledProgram>> installed_programs() const override;
};

}  // namespace myclean::platform
