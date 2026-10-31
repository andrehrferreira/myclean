#include "core/space/installers.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::file;
using testing::scan_ok;
namespace fs = std::filesystem;

const fs::path downloads{L"C:\\Users\\owner\\Downloads"};

class FakeInspector final : public IInstallerInspector {
 public:
  std::map<fs::path, std::string> product_codes;
  std::set<fs::path> broken;
  std::map<std::string, std::string> installed;  // product code -> name
  std::map<fs::path, VersionInfo> versions;
  std::vector<InstalledProgram> programs;
  bool programs_fail = false;
  mutable std::set<fs::path> opened;

  [[nodiscard]] Result<std::optional<std::string>> msi_product_code(
      const fs::path& package) const override {
    opened.insert(package);
    if (broken.contains(package)) {
      return fail(Errc::invalid_data, "not a package");
    }
    const auto code = product_codes.find(package);
    return code == product_codes.end() ? std::nullopt : std::optional{code->second};
  }
  [[nodiscard]] Result<std::optional<std::string>> installed_msi_product(
      const std::string& product_code) const override {
    const auto product = installed.find(product_code);
    return product == installed.end() ? std::nullopt : std::optional{product->second};
  }
  [[nodiscard]] Result<std::optional<VersionInfo>> exe_version(
      const fs::path& executable) const override {
    opened.insert(executable);
    const auto version = versions.find(executable);
    return version == versions.end() ? std::nullopt : std::optional{version->second};
  }
  [[nodiscard]] Result<std::vector<InstalledProgram>> installed_programs() const override {
    if (programs_fail) {
      return fail(Errc::access_denied, "registry");
    }
    return programs;
  }
};

TEST(MatchesProgram, MatchesProductAndVendor) {
  EXPECT_TRUE(matches_program(VersionInfo{.product_name = "App", .company_name = "Vendor"},
                              InstalledProgram{.display_name = "App", .publisher = "Vendor"}));
}

TEST(MatchesProgram, IgnoresVersionsArchitectureAndLegalSuffixes) {
  EXPECT_TRUE(matches_program(
      VersionInfo{.product_name = "7-Zip", .company_name = "Igor Pavlov"},
      InstalledProgram{.display_name = "7-Zip 23.01 (x64)", .publisher = "Igor Pavlov"}));
  EXPECT_TRUE(
      matches_program(VersionInfo{.product_name = "Visual Studio Code Setup",
                                  .company_name = "Microsoft Corporation"},
                      InstalledProgram{.display_name = "Microsoft Visual Studio Code (User)",
                                       .publisher = "Microsoft Corporation"}));
  EXPECT_TRUE(matches_program(
      VersionInfo{.product_name = "Git", .company_name = "The Git Development Community"},
      InstalledProgram{.display_name = "Git", .publisher = "The Git Development Community"}));
}

TEST(MatchesProgram, RejectsAnotherVendorOrProductOrMissingNames) {
  EXPECT_FALSE(matches_program(VersionInfo{.product_name = "App", .company_name = "Vendor"},
                               InstalledProgram{.display_name = "App", .publisher = "Other"}));
  EXPECT_FALSE(matches_program(VersionInfo{.product_name = "App", .company_name = "Vendor"},
                               InstalledProgram{.display_name = "Tool", .publisher = "Vendor"}));
  EXPECT_FALSE(matches_program(VersionInfo{.product_name = "App", .company_name = ""},
                               InstalledProgram{.display_name = "App", .publisher = ""}));
  EXPECT_FALSE(matches_program(VersionInfo{.product_name = "Setup", .company_name = "Vendor"},
                               InstalledProgram{.display_name = "App", .publisher = "Vendor"}));
}

TEST(UsedInstallers, ListsInstalledPackagesAndMatchedExecutables) {
  FakeDirectoryReader reader;
  ListedEntry cloud = file("cloud.exe", 0, 9);
  cloud.attributes.recall_on_data_access = true;
  reader.listings[downloads] = {file("tool.msi", 3000, 1),   file("old.msi", 2000, 2),
                                file("broken.msi", 10, 3),   file("AppSetup.exe", 5000, 4),
                                file("unknown.exe", 100, 5), file("notes.txt", 10, 6),
                                file("TOOL2.MSI", 50, 7),    cloud};
  FakeInspector inspector;
  inspector.product_codes[downloads / "tool.msi"] = "{11111111-1111-1111-1111-111111111111}";
  inspector.product_codes[downloads / "old.msi"] = "{22222222-2222-2222-2222-222222222222}";
  inspector.product_codes[downloads / "TOOL2.MSI"] = "{11111111-1111-1111-1111-111111111111}";
  inspector.broken.insert(downloads / "broken.msi");
  inspector.installed["{11111111-1111-1111-1111-111111111111}"] = "Tool 1.0";
  inspector.versions[downloads / "AppSetup.exe"] = {.product_name = "App",
                                                    .company_name = "Vendor"};
  inspector.versions[downloads / "unknown.exe"] = {.product_name = "X", .company_name = "Y"};
  inspector.programs = {{.display_name = "Other", .publisher = "Vendor"},
                        {.display_name = "App", .publisher = "Vendor", .display_version = "2"}};

  const Result<InstallerReport> report = used_installers(scan_ok(reader, downloads), inspector);

  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->used.size(), 3U);
  EXPECT_EQ(report->used[0].path, downloads / "AppSetup.exe");
  EXPECT_EQ(report->used[0].kind, InstallerKind::exe);
  EXPECT_EQ(report->used[0].installed_as, "App");
  EXPECT_EQ(report->used[0].publisher, "Vendor");
  EXPECT_EQ(report->used[1].path, downloads / "tool.msi");
  EXPECT_EQ(report->used[1].kind, InstallerKind::msi);
  EXPECT_EQ(report->used[1].size_on_disk, Bytes{3000});
  EXPECT_EQ(report->used[1].installed_as, "Tool 1.0");
  EXPECT_EQ(report->used[1].product_code, "{11111111-1111-1111-1111-111111111111}");
  EXPECT_EQ(report->used[2].path, downloads / "TOOL2.MSI");
  ASSERT_EQ(report->errors.size(), 1U);
  EXPECT_EQ(report->errors[0].path, downloads / "broken.msi");
  EXPECT_FALSE(inspector.opened.contains(downloads / "cloud.exe"));
  EXPECT_FALSE(inspector.opened.contains(downloads / "notes.txt"));
}

TEST(UsedInstallers, FailsWhenInstalledProgramsCannotBeListed) {
  FakeDirectoryReader reader;
  reader.listings[downloads] = {file("a.exe", 1, 1)};
  FakeInspector inspector;
  inspector.programs_fail = true;

  EXPECT_EQ(used_installers(scan_ok(reader, downloads), inspector).error().code,
            Errc::access_denied);
}

}  // namespace
}  // namespace myclean::space
