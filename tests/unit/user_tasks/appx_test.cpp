#include "core/user_tasks/appx.hpp"

#include <chrono>
#include <filesystem>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../placement/placement_fakes.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/user_tasks/task.hpp"
#include "core/user_tasks/uninstall.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::user_tasks {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

const std::string package = "Vendor.Notes_2.0.0.0_x64__abc";

// Removes nothing on the machine: it only forgets the package.
class FakeRemover final : public IPackageRemover {
 public:
  std::set<std::string> packages{package};
  std::vector<std::string> removed;

  [[nodiscard]] Result<bool> installed(const std::string& full_name) const override {
    return packages.contains(full_name);
  }
  [[nodiscard]] Result<void> remove(const std::string& full_name,
                                    const std::stop_token& /*stop*/) override {
    removed.push_back(full_name);
    packages.erase(full_name);
    return {};
  }
};

// Starts nothing; it drops the Uninstall entry as a finished uninstaller would.
class DroppingLauncher final : public IUninstallLauncher {
 public:
  explicit DroppingLauncher(placement::testing::FakeRegistry& registry, std::string key)
      : registry_(&registry), key_(std::move(key)) {}

  [[nodiscard]] Result<void> launch(const fs::path& /*program*/, std::string_view /*arguments*/,
                                    std::chrono::minutes /*wait*/,
                                    const std::stop_token& /*stop*/) override {
    registry_->drop(key_);
    return {};
  }

 private:
  placement::testing::FakeRegistry* registry_;
  std::string key_;
};

Json appx_task() {
  return Json{{"plan", 41},
              {"title", "Uninstall Notes"},
              {"kind", "uninstall"},
              {"operation", "apply"},
              {"tier", "ask"},
              {"action", {{"package", package}, {"name", "Notes"}}},
              {"items", Json::array({Json{{"id", 402},
                                          {"path", "uninstall:Notes"},
                                          {"bytes", 0},
                                          {"approved", true},
                                          {"volume_serial", 0},
                                          {"file_id", ""}}})}};
}

// Apps spec, "Uninstall Through The App's Own Uninstaller After Approval": an Appx package is
// removed through the package manager, and the removal is verified.
TEST(AppxRemovalTest, TheTrayRemovesAnApprovedPackageAndVerifiesItIsGone) {
  const Result<UserTask> parsed = parse_task(appx_task());
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  const UserTask task = parsed.value_or(UserTask{});
  EXPECT_EQ(task.program_package, package);
  EXPECT_TRUE(task.program_key.empty());

  EXPECT_EQ(run(task, Toolbox{}, {}).state, PlanState::failed);  // no package manager
  FakeRemover remover;
  const TaskReport report = run(task, Toolbox{.packages = &remover}, {});
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_EQ(remover.removed, std::vector<std::string>{package});
  EXPECT_FALSE(report.undo.reversible());
}

TEST(AppxRemovalTest, APackageGoneMeanwhileIsNotRemoved) {
  FakeRemover remover;
  remover.packages.clear();
  AppxRemoval removal(remover, package, "Notes", {});
  EXPECT_EQ(removal.preview().error().code, Errc::not_found);
  EXPECT_TRUE(remover.removed.empty());
  EXPECT_EQ(removal.undo(Undo{}).error().code, Errc::not_reversible);
}

// Spec "Program folder is never deleted by hand": the leftover folder is reported, not deleted.
TEST(UninstallLeftoverTest, AFolderLeftBehindIsReportedAndKept) {
  const std::string key = R"(HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\A)";
  placement::testing::FakeRegistry registry;
  registry.put(key, "DisplayName", placement::RegistryValue{.text = "A"});
  registry.put(key, "UninstallString",
               placement::RegistryValue{.text = R"("C:\Program Files\A\unins000.exe")"});
  registry.put(key, "InstallLocation", placement::RegistryValue{.text = R"(C:\Program Files\A)"});
  fakes::FakeFileSystem files;
  files.add_directory(L"C:\\Program Files\\A");
  DroppingLauncher launcher(registry, key);
  UninstallAction uninstalling(registry, launcher, key, "A", {}, &files);
  Result<Preview> preview = uninstalling.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  Preview applied = preview.value_or(Preview{});
  applied.items.front().id = PlanItemId{5};
  const Result<Undo> undo = uninstalling.apply(applied);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  const Result<Verification> verified = uninstalling.verify(undo.value_or(Undo{}));
  ASSERT_FALSE(verified.has_value());
  EXPECT_NE(verified.error().message.find(R"(C:\Program Files\A was left behind)"),
            std::string::npos)
      << verified.error().message;
  EXPECT_TRUE(files.stat(L"C:\\Program Files\\A").has_value());
}

}  // namespace
}  // namespace myclean::user_tasks
