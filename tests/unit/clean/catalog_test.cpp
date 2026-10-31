#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/clean/workspaces.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "fixture.hpp"

// The shipped clean cases (data/packs/*/clean.toml) and data/workspaces.toml.
namespace myclean::clean {
namespace {

using namespace myclean::clean::testing;
using rules::CleaningRule;
using rules::Strategy;

TEST(ShippedCatalog, EveryCategoryHasCleanCases) {
  std::map<std::string, int> per_category;
  for (const CleaningRule& rule : shipped_rules()) {
    ++per_category[rule.category];
  }
  for (const char* category : {"windows", "browsers", "electron", "ides", "gpu-shader-caches",
                               "game-launchers", "package-managers", "dev-artifacts"}) {
    EXPECT_GT(per_category[category], 0) << category;
  }
}

// Cleaning spec, "Shader caches default to ask".
TEST(ShippedCatalog, ShaderCachesAreAsk) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.category == "gpu-shader-caches") {
      EXPECT_EQ(rule.tier, Tier::ask) << rule.id;
    }
  }
  const CleaningRule& nvidia = shipped_rule("gpu/nvidia-dxcache");
  EXPECT_EQ(nvidia.targets.front(), R"(%LOCALAPPDATA%\NVIDIA\DXCache\*)");
}

// Analysis F-013 and the cleaning spec: global package caches use their tool's command, under
// the owner's token, and wait for approval.
TEST(ShippedCatalog, PackageManagersAreNativeUserAsk) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.strategy == Strategy::native && rule.category == "package-managers") {
      EXPECT_EQ(rule.scope, ExecutionContext::user) << rule.id;
      EXPECT_EQ(rule.tier, Tier::ask) << rule.id;
    }
  }
  EXPECT_EQ(shipped_rule("node/pnpm-store").command, "pnpm store prune");
  EXPECT_EQ(shipped_rule("windows/component-store").command,
            "DISM /Online /Cleanup-Image /StartComponentCleanup");
}

// docs/spikes/X3-cleanup-handlers.md: `handler` names a VolumeCaches key; handlers that look
// at the owner's profile run in the tray (scope user), the others in the service; DownloadsFolder
// (the owner's Downloads) and "User file versions" (File History) are never offered; no handler
// runs without approval.
TEST(ShippedCatalog, HandlersHaveTheirScopeAndForbiddenOnesAreAbsent) {
  const std::set<std::string, std::less<>> user_handlers = {
      "Temporary Files",           "Thumbnail Cache",
      "Internet Cache Files",      "D3D Shader Cache",
      "Downloaded Program Files",  "Offline Pages Files",
      "Active Setup Temp Folders", "Recycle Bin"};
  int handlers = 0;
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.strategy != Strategy::handler) {
      continue;
    }
    ++handlers;
    EXPECT_NE(rule.handler, "DownloadsFolder") << rule.id;
    EXPECT_NE(rule.handler, "User file versions") << rule.id;
    EXPECT_EQ(rule.scope, user_handlers.contains(rule.handler) ? ExecutionContext::user
                                                               : ExecutionContext::service)
        << rule.id;
    EXPECT_EQ(rule.tier, Tier::ask) << rule.id;
  }
  EXPECT_GT(handlers, 0);
  EXPECT_EQ(shipped_rule("windows/update-cleanup").handler, "Update Cleanup");
}

// Cleaning spec, "Browser rule limits": no browser or Electron target names profile data.
TEST(ShippedCatalog, BrowserAndElectronTargetsAreCachesOnly) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.category != "browsers" && rule.category != "electron") {
      continue;
    }
    for (const std::string& target : rule.targets) {
      for (const std::string_view forbidden :
           {"Cookies", "History", "Login Data", "Session", "Local Storage", "IndexedDB",
            "Preferences", "Network", "Web Data"}) {
        EXPECT_EQ(target.find(forbidden), std::string::npos) << rule.id << ": " << target;
      }
    }
  }
}

// Cleaning spec, "Developer Artifact Qualification": every artifact rule requires git, no use
// and inactivity, and auto-tier ones wait at least 30 days.
TEST(ShippedCatalog, ArtifactRulesRequireEveryQualification) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (!rule.project_marker) {
      continue;
    }
    EXPECT_TRUE(rule.require.git_ignored) << rule.id;
    EXPECT_TRUE(rule.require.not_in_use) << rule.id;
    ASSERT_TRUE(rule.require.inactive_days) << rule.id;
    EXPECT_GE(*rule.require.inactive_days, rule.tier == Tier::automatic ? 30 : 14) << rule.id;
    for (const std::string& target : rule.targets) {
      EXPECT_NE(target, "Saved") << rule.id;
      EXPECT_NE(target, ".venv") << rule.id;
    }
  }
}

TEST(ShippedWorkspaces, DefaultRootsAreFAndW) {
  const std::filesystem::path file = std::filesystem::path{MYCLEAN_DATA_DIR} / "workspaces.toml";
  const Result<std::string> text = rules::DiskDataFiles{}.read(file);
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const Result<Workspaces> workspaces = parse_workspace_file(*text, file);
  ASSERT_TRUE(workspaces.has_value()) << workspaces.error().message;
  EXPECT_EQ(workspaces->roots, (std::vector<std::filesystem::path>{R"(F:\)", R"(W:\)"}));
  EXPECT_NE(std::ranges::find(workspaces->skip, L"node_modules"), workspaces->skip.end());
  EXPECT_NE(std::ranges::find(workspaces->skip, L".git"), workspaces->skip.end());
}

}  // namespace
}  // namespace myclean::clean
