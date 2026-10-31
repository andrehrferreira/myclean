#include "platform/scan/installer_inspector.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <msi.h>
#include <msiquery.h>

#include "core/model/result.hpp"
#include "core/space/installers.hpp"
#include "core/space/scanner.hpp"
#include "platform/scan/directory_reader.hpp"
#include "platform/scan/scan_support.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

const std::string absent_code = "{6D1A6B7E-0C59-4E52-9C2C-7A1F0F3B9E11}";

bool run(MSIHANDLE database, const std::wstring& query) {
  PMSIHANDLE view;
  return ::MsiDatabaseOpenViewW(database, query.c_str(), &view) == ERROR_SUCCESS &&
         ::MsiViewExecute(view, 0) == ERROR_SUCCESS;
}

// Writes a minimal package (only a Property table) to `path`; no ProductCode when `code` is
// empty.
bool make_msi(const fs::path& path, const std::string& code) {
  PMSIHANDLE database;
  if (::MsiOpenDatabaseW(path.c_str(), MSIDBOPEN_CREATE, &database) != ERROR_SUCCESS) {
    return false;
  }
  if (!run(database,
           L"CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL "
           L"LOCALIZABLE PRIMARY KEY `Property`)") ||
      !run(database,
           L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('Manufacturer', 'Me')")) {
    return false;
  }
  if (!code.empty() &&
      !run(database, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductCode', '" +
                         scan_support::wide(code) + L"')")) {
    return false;
  }
  return ::MsiDatabaseCommit(database) == ERROR_SUCCESS;
}

// The ProductCode of some product installed on this machine, if any (read-only).
std::optional<std::string> installed_product_code() {
  std::array<wchar_t, 39> code{};
  if (::MsiEnumProductsW(0, code.data()) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return scan_support::narrow(std::wstring(code.data()));
}

TEST(Win32InstallerInspector, ReadsTheProductCodeOfAPackage) {
  const Sandbox sandbox;
  ASSERT_TRUE(make_msi(sandbox.root() / "tool.msi", absent_code));
  ASSERT_TRUE(make_msi(sandbox.root() / "nocode.msi", ""));
  write_file(sandbox.root() / "fake.msi", 100);
  const Win32InstallerInspector inspector;

  EXPECT_EQ(inspector.msi_product_code(sandbox.root() / "tool.msi").value(), absent_code);
  EXPECT_EQ(inspector.msi_product_code(sandbox.root() / "nocode.msi").value(), std::nullopt);
  EXPECT_FALSE(inspector.msi_product_code(sandbox.root() / "fake.msi").has_value());
}

TEST(Win32InstallerInspector, TellsWhetherAProductIsInstalled) {
  const Win32InstallerInspector inspector;

  EXPECT_EQ(inspector.installed_msi_product(absent_code).value(), std::nullopt);
  EXPECT_EQ(inspector.installed_msi_product("not a guid").error().code, Errc::invalid_data);
  const std::optional<std::string> installed = installed_product_code();
  if (!installed) {
    GTEST_SKIP() << "no Windows Installer product on this machine";
  }
  const auto product = inspector.installed_msi_product(*installed);
  ASSERT_TRUE(product.has_value());
  ASSERT_TRUE(product->has_value());
  EXPECT_FALSE(product->value_or("").empty());
}

TEST(Win32InstallerInspector, ReadsVersionInfoOfAnExecutable) {
  std::wstring system(MAX_PATH, L'\0');
  system.resize(::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())));
  const Sandbox sandbox;
  write_file(sandbox.root() / "text.exe", 100);
  const Win32InstallerInspector inspector;

  const auto version = inspector.exe_version(fs::path{system} / L"cmd.exe");

  ASSERT_TRUE(version.has_value()) << version.error().message;
  ASSERT_TRUE(version->has_value());
  const space::VersionInfo info = version->value_or(space::VersionInfo{});
  EXPECT_NE(info.company_name.find("Microsoft"), std::string::npos);
  EXPECT_FALSE(info.product_name.empty());
  EXPECT_EQ(inspector.exe_version(sandbox.root() / "text.exe").value(), std::nullopt);
}

TEST(Win32InstallerInspector, ListsInstalledPrograms) {
  const Win32InstallerInspector inspector;

  const auto programs = inspector.installed_programs();

  ASSERT_TRUE(programs.has_value()) << programs.error().message;
  EXPECT_FALSE(programs->empty());
  for (const space::InstalledProgram& program : *programs) {
    EXPECT_FALSE(program.display_name.empty());
  }
}

TEST(Win32InstallerInspector, FindsAPackageWhoseProductIsInstalled) {
  const std::optional<std::string> installed = installed_product_code();
  if (!installed) {
    GTEST_SKIP() << "no Windows Installer product on this machine";
  }
  const Sandbox sandbox;
  ASSERT_TRUE(make_msi(sandbox.root() / "used.msi", *installed));
  ASSERT_TRUE(make_msi(sandbox.root() / "unused.msi", absent_code));
  const Win32DirectoryReader directories;
  const Result<space::ScanResult> scan = space::scan(directories, sandbox.root());
  ASSERT_TRUE(scan.has_value());

  const Result<space::InstallerReport> report =
      space::used_installers(*scan, Win32InstallerInspector{});

  ASSERT_TRUE(report.has_value()) << report.error().message;
  ASSERT_EQ(report->used.size(), 1U);
  EXPECT_EQ(report->used[0].path, sandbox.root() / "used.msi");
  EXPECT_EQ(report->used[0].product_code, *installed);
  EXPECT_TRUE(report->errors.empty());
}

}  // namespace
}  // namespace myclean::platform
