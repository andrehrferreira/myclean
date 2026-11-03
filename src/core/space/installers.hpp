#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Installers already used (space spec, "Installers Already Used"): an .msi whose ProductCode is
// installed, an .exe whose version-info ProductName and CompanyName match an installed
// program's Uninstall entry. Read-only.
namespace myclean::space {

// The version resource of an executable.
struct VersionInfo {
  std::string product_name;
  std::string company_name;
};

// One Uninstall registry entry (HKLM 64-bit and 32-bit views, HKCU).
struct InstalledProgram {
  std::string display_name;
  std::string publisher;
  std::string display_version;
};

// Reads installer files and the installed-programs list. Implemented by src/platform/scan.
class IInstallerInspector {
 public:
  virtual ~IInstallerInspector() = default;
  IInstallerInspector(const IInstallerInspector&) = delete;
  IInstallerInspector& operator=(const IInstallerInspector&) = delete;
  IInstallerInspector(IInstallerInspector&&) = delete;
  IInstallerInspector& operator=(IInstallerInspector&&) = delete;

  // The ProductCode property of an .msi package; nullopt when it has none.
  [[nodiscard]] virtual Result<std::optional<std::string>> msi_product_code(
      const std::filesystem::path& package) const = 0;
  // The name of the installed product with `product_code`; nullopt when it is not installed.
  [[nodiscard]] virtual Result<std::optional<std::string>> installed_msi_product(
      const std::string& product_code) const = 0;
  // The version resource of an executable; nullopt when it has none.
  [[nodiscard]] virtual Result<std::optional<VersionInfo>> exe_version(
      const std::filesystem::path& executable) const = 0;
  [[nodiscard]] virtual Result<std::vector<InstalledProgram>> installed_programs() const = 0;

 protected:
  IInstallerInspector() = default;
};

enum class InstallerKind { msi, exe };

[[nodiscard]] std::string_view to_string(InstallerKind kind);

struct UsedInstaller {
  std::filesystem::path path;
  InstallerKind kind = InstallerKind::msi;
  Bytes size;
  Bytes size_on_disk;
  std::string product_code;  // .msi only
  std::string installed_as;  // the installed product's name
  std::string publisher;     // the matching Uninstall entry's publisher (.exe only)
};

struct InstallerReport {
  std::vector<UsedInstaller> used;  // largest on disk first
  std::vector<ItemError> errors;
};

// True when the version info of an .exe names `program`: the significant words of the product
// name appear in order in the display name (or the other way round) and make up at least half
// of it, and the company and the
// publisher name the same vendor. Case, punctuation, version numbers, architecture tags and
// legal suffixes (Inc., Ltd., Corporation, ...) are ignored.
[[nodiscard]] bool matches_program(const VersionInfo& version, const InstalledProgram& program);

// The used .msi and .exe files of `scan`. Links, extra hard links and cloud placeholders are
// skipped (reading them could download them). Fails only when the installed programs cannot be
// listed.
[[nodiscard]] Result<InstallerReport> used_installers(const ScanResult& scan,
                                                      const IInstallerInspector& inspector);

}  // namespace myclean::space
