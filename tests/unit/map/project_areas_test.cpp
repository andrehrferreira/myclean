#include "core/map/project_areas.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/map/attribution.hpp"
#include "core/rules/schema.hpp"
#include "core/space/scanner.hpp"
#include "space/space_fakes.hpp"

namespace myclean::map {
namespace {

using space::testing::file;
using space::testing::folder;
using space::testing::scan_ok;

rules::CleaningRule project_rule(std::string id, std::string marker,
                                 std::vector<std::string> targets,
                                 std::optional<std::string> contains = std::nullopt) {
  rules::CleaningRule rule{.id = std::move(id),
                           .category = "dev-artifacts",
                           .targets = std::move(targets),
                           .project_marker = std::move(marker)};
  rule.require.contains = std::move(contains);
  return rule;
}

// E:\ with a Cargo project (a target with an incremental cache), a CMake project with two build
// trees, a FetchContent folder and a build.zig file, a node project, and a vendored crate inside
// node_modules that must not count as a project.
space::ScanResult scan() {
  space::testing::FakeDirectoryReader reader;
  reader.listings[R"(E:\)"] = {folder("synap"), folder("engine"), folder("web")};
  reader.listings[R"(E:\synap)"] = {file("Cargo.toml", 10, 1), folder("target"), folder("src")};
  reader.listings[R"(E:\synap\src)"] = {file("main.rs", 10, 2)};
  reader.listings[R"(E:\synap\target)"] = {folder("debug")};
  reader.listings[R"(E:\synap\target\debug)"] = {folder("incremental"), file("synap.exe", 500, 3)};
  reader.listings[R"(E:\synap\target\debug\incremental)"] = {folder("synap-1")};
  reader.listings[R"(E:\synap\target\debug\incremental\synap-1)"] = {file("dep.bin", 900, 4)};
  reader.listings[R"(E:\engine)"] = {file("CMakeLists.txt", 10, 5), file("build.zig", 10, 6),
                                     folder("build")};
  reader.listings[R"(E:\engine\build)"] = {folder("asan"), folder("_deps")};
  reader.listings[R"(E:\engine\build\asan)"] = {file("CMakeCache.txt", 10, 7),
                                                file("engine.exe", 700, 8)};
  reader.listings[R"(E:\engine\build\_deps)"] = {folder("fmt")};
  reader.listings[R"(E:\engine\build\_deps\fmt)"] = {file("CMakeLists.txt", 10, 9),
                                                     folder("build")};
  reader.listings[R"(E:\engine\build\_deps\fmt\build)"] = {file("fmt.lib", 30, 10)};
  reader.listings[R"(E:\web)"] = {file("package.json", 10, 11), folder("node_modules")};
  reader.listings[R"(E:\web\node_modules)"] = {folder("left-pad")};
  reader.listings[R"(E:\web\node_modules\left-pad)"] = {file("package.json", 10, 12),
                                                        folder("node_modules")};
  reader.listings[R"(E:\web\node_modules\left-pad\node_modules)"] = {file("x.js", 10, 13)};
  return scan_ok(reader, R"(E:\)");
}

std::vector<rules::CleaningRule> rules() {
  return {project_rule("rust/target", "Cargo.toml", {"target"}),
          project_rule("rust/stale-incremental", "Cargo.toml", {R"(target\*\incremental\*)"}),
          project_rule("cpp/cmake-artifacts", "CMakeLists.txt", {"build*"}),
          project_rule("cpp/stale-build-tree", "CMakeLists.txt", {R"(build\*)"}, "CMakeCache.txt"),
          project_rule("node/node-modules", "package.json", {"node_modules"}),
          rules::CleaningRule{.id = "windows/temp", .category = "temp", .targets = {R"(%TEMP%)"}}};
}

std::vector<std::wstring> paths_of(const std::vector<Area>& areas, const std::string& id) {
  for (const Area& area : areas) {
    if (area.id == id) {
      return area.paths;
    }
  }
  return {};
}

TEST(ProjectAreas, NameTheArtifactFoldersOfEveryProjectInTheScan) {
  const std::vector<Area> areas =
      project_areas(scan(), rules(), std::vector<std::wstring>{L".git"});

  ASSERT_EQ(areas.size(), 5U);
  EXPECT_EQ(paths_of(areas, "rust/target"), (std::vector<std::wstring>{LR"(E:\synap\target)"}));
  EXPECT_EQ(paths_of(areas, "rust/stale-incremental"),
            (std::vector<std::wstring>{LR"(E:\synap\target\debug\incremental\synap-1)"}));
  // build.zig is a file, not an artifact; only the build tree holding CMakeCache.txt is one, and
  // the CMake project inside build\_deps is never searched.
  EXPECT_EQ(paths_of(areas, "cpp/cmake-artifacts"),
            (std::vector<std::wstring>{LR"(E:\engine\build)"}));
  EXPECT_EQ(paths_of(areas, "cpp/stale-build-tree"),
            (std::vector<std::wstring>{LR"(E:\engine\build\asan)"}));
  // The package inside node_modules is not a project of its own.
  EXPECT_EQ(paths_of(areas, "node/node-modules"),
            (std::vector<std::wstring>{LR"(E:\web\node_modules)"}));

  const Area& target = areas.front();
  EXPECT_EQ(target.category, Category::build_artifacts);
  EXPECT_EQ(target.reclaim.kind, ReclaimKind::clean_rule);
  EXPECT_EQ(target.reclaim.ref, "rust/target");
}

TEST(ProjectAreas, SkippedFoldersAreNotSearched) {
  const std::vector<Area> areas =
      project_areas(scan(), rules(), std::vector<std::wstring>{L"synap", L"web"});

  EXPECT_TRUE(paths_of(areas, "rust/target").empty());
  EXPECT_TRUE(paths_of(areas, "node/node-modules").empty());
  EXPECT_EQ(paths_of(areas, "cpp/cmake-artifacts"),
            (std::vector<std::wstring>{LR"(E:\engine\build)"}));
}

}  // namespace
}  // namespace myclean::map
