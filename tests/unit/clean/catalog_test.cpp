#include <algorithm>
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

// Cleaning spec, "Shader caches are auto".
TEST(ShippedCatalog, ShaderCachesAreAuto) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.category == "gpu-shader-caches") {
      EXPECT_EQ(rule.tier, Tier::automatic) << rule.id;
      EXPECT_TRUE(rule.require.not_in_use) << rule.id;
    }
  }
  const CleaningRule& nvidia = shipped_rule("gpu/nvidia-dxcache");
  EXPECT_EQ(nvidia.targets.front(), R"(%LOCALAPPDATA%\NVIDIA\DXCache\*)");
  // Steam's pre-cache in every library of libraryfolders.vdf, a week after its last write.
  const CleaningRule& steam = shipped_rule("games/steam-library-shadercache");
  EXPECT_EQ(steam.targets, std::vector<std::string>{R"(%STEAM_LIBRARY%\steamapps\shadercache\*)"});
  EXPECT_EQ(steam.require.min_age_days, 7);
}

// A copy of Docker Desktop's data folder may hold the only copy of a volume: the owner decides,
// and the folders Docker's settings name are never offered.
TEST(ShippedCatalog, DockerDataCopiesAreAskedAndKeepTheNamedFolders) {
  const CleaningRule& copy = shipped_rule("wsl-docker/docker-data-copy");
  EXPECT_EQ(copy.tier, Tier::ask);
  EXPECT_EQ(copy.require.outside_of, "%DOCKER_WSL_DATA%");
  EXPECT_EQ(copy.require.contains, R"(disk\docker_data.vhdx)");
  EXPECT_TRUE(copy.require.not_in_use);
  EXPECT_GE(copy.require.min_age_days.value_or(0), 30);
}

// Analysis F-013 and the cleaning spec: global package caches use their tool's command, under
// the owner's token, and run on schedule (the tool downloads what it misses); Docker's prune,
// which also removes stopped containers, waits for approval.
TEST(ShippedCatalog, PackageManagersAreNativeUserAuto) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.strategy == Strategy::native && rule.category == "package-managers") {
      EXPECT_EQ(rule.scope, ExecutionContext::user) << rule.id;
      EXPECT_EQ(rule.tier, Tier::automatic) << rule.id;
    }
  }
  EXPECT_EQ(shipped_rule("wsl-docker/docker-prune").tier, Tier::ask);
  EXPECT_EQ(shipped_rule("node/pnpm-store").command, "pnpm store prune");
  EXPECT_EQ(shipped_rule("windows/component-store").command,
            "DISM /Online /Cleanup-Image /StartComponentCleanup");
}

// docs/spikes/X3-cleanup-handlers.md: `handler` names a VolumeCaches key; handlers that look
// at the owner's profile run in the tray (scope user), the others in the service; DownloadsFolder
// (the owner's Downloads) and "User file versions" (File History) are never offered; only the
// handlers that remove nothing but caches and setup leftovers run without approval (cleaning
// spec, "Handlers of regenerable data are auto").
TEST(ShippedCatalog, HandlersHaveTheirScopeAndForbiddenOnesAreAbsent) {
  const std::set<std::string, std::less<>> user_handlers = {
      "Temporary Files",           "Thumbnail Cache",
      "Internet Cache Files",      "D3D Shader Cache",
      "Downloaded Program Files",  "Offline Pages Files",
      "Active Setup Temp Folders", "Recycle Bin"};
  const std::set<std::string, std::less<>> auto_handlers = {
      "Thumbnail Cache", "Internet Cache Files", "Delivery Optimization Files",
      "Temporary Setup Files", "Upgrade Discarded Files"};
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
    if (rule.require.min_age_days) {
      continue;  // the aged Recycle Bin items, below
    }
    EXPECT_EQ(rule.tier, auto_handlers.contains(rule.handler) ? Tier::automatic : Tier::ask)
        << rule.id;
  }
  EXPECT_GT(handlers, 0);
  EXPECT_EQ(shipped_rule("windows/recycle-bin").tier, Tier::ask);
  EXPECT_EQ(shipped_rule("windows/memory-dumps").tier, Tier::ask);
  EXPECT_EQ(shipped_rule("windows/update-cleanup").handler, "Update Cleanup");
}

// Cleaning spec, "Aged Recycle Bin Items": of the handler cases, only the Recycle Bin's sets a
// requirement, min_age_days, counted from each item's deletion time; it is auto only past the
// auto tier's floor of 7 days. The whole bin stays ask.
TEST(ShippedCatalog, OnlyTheAgedRecycleBinCaseSetsAHandlerRequirement) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (rule.strategy != Strategy::handler || rule.handler == "Recycle Bin") {
      continue;
    }
    EXPECT_FALSE(rule.require.min_age_days) << rule.id;
    EXPECT_FALSE(rule.require.inactive_days) << rule.id;
    EXPECT_FALSE(rule.require.not_in_use || rule.require.git_ignored) << rule.id;
  }
  const CleaningRule& aged = shipped_rule("windows/recycle-bin-aged");
  EXPECT_EQ(aged.strategy, Strategy::handler);
  EXPECT_EQ(aged.handler, "Recycle Bin");
  EXPECT_EQ(aged.scope, ExecutionContext::user);
  EXPECT_EQ(aged.tier, Tier::automatic);
  EXPECT_EQ(aged.require.min_age_days, 30);
  EXPECT_FALSE(shipped_rule("windows/recycle-bin").require.min_age_days);
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
// and inactivity; auto-tier ones wait at least 7 days, for the project or for the item itself.
// Invariant for outside_git (a project outside any git work tree may be offered): only together
// with git_ignored (inside a work tree git still vouches), and only on auto-tier pure caches,
// whose loss costs nothing but a rebuild, that also wait 7 days for the cache itself
// (min_age_days). Their targets never end in a name a person may give a folder by hand (build
// output such as bin or Binaries can be all a project without its sources has).
TEST(ShippedCatalog, ArtifactRulesRequireEveryQualification) {
  for (const CleaningRule& rule : shipped_rules()) {
    if (!rule.project_marker) {
      continue;
    }
    EXPECT_TRUE(rule.require.git_ignored) << rule.id;
    if (rule.require.outside_git) {
      EXPECT_EQ(rule.tier, Tier::automatic) << rule.id;
      EXPECT_GE(rule.require.min_age_days.value_or(0), 7) << rule.id;
      for (const std::string& target : rule.targets) {
        const std::string name = std::filesystem::path{target}.filename().string();
        for (const std::string_view hand_made :
             {"build", "dist", "out", "bin", "obj", "Binaries", "target", "Saved", "lib"}) {
          EXPECT_NE(name, hand_made) << rule.id << ": " << target;
        }
      }
    }
    EXPECT_TRUE(rule.require.not_in_use) << rule.id;
    ASSERT_TRUE(rule.require.inactive_days) << rule.id;
    if (rule.tier == Tier::automatic) {
      EXPECT_GE(std::max(*rule.require.inactive_days, rule.require.min_age_days.value_or(0)), 7)
          << rule.id;
    } else {
      EXPECT_GE(*rule.require.inactive_days, 14) << rule.id;
    }
    for (const std::string& target : rule.targets) {
      EXPECT_NE(target, "Saved") << rule.id;
      EXPECT_NE(target, ".venv") << rule.id;
    }
  }
}

TEST(ShippedWorkspaces, DefaultRootsAreTheWorkVolumes) {
  const std::filesystem::path file = std::filesystem::path{MYCLEAN_DATA_DIR} / "workspaces.toml";
  const Result<std::string> text = rules::DiskDataFiles{}.read(file);
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const Result<Workspaces> workspaces = parse_workspace_file(*text, file);
  ASSERT_TRUE(workspaces.has_value()) << workspaces.error().message;
  EXPECT_EQ(workspaces->roots,
            (std::vector<std::filesystem::path>{R"(D:\)", R"(E:\)", R"(F:\)", R"(G:\)", R"(W:\)"}));
  EXPECT_NE(std::ranges::find(workspaces->skip, L"node_modules"), workspaces->skip.end());
  EXPECT_NE(std::ranges::find(workspaces->skip, L".git"), workspaces->skip.end());
}

}  // namespace
}  // namespace myclean::clean
