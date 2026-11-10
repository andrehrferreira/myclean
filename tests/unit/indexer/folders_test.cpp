#include "core/indexer/folders.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clean/clean_fakes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/indexer/scope.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/rules/schema.hpp"
#include "fake_scope.hpp"

namespace myclean::indexer {
namespace {

using clean::testing::FakeEnvironment;
using clean::testing::FakeTree;
using testing::FakeScope;

// The node pack's project case and two areas of the packs, as the registry hands them over.
std::vector<rules::CleaningRule> project_rules() {
  return {rules::CleaningRule{.id = "artifacts",
                              .category = "dev-artifacts",
                              .targets = {"node_modules", "dist"},
                              .project_marker = "package.json"},
          rules::CleaningRule{.id = "target",
                              .category = "dev-artifacts",
                              .targets = {"target"},
                              .project_marker = "Cargo.toml",
                              .require = {.contains = std::string("CACHEDIR.TAG")}},
          // Not a project case: ignored here.
          rules::CleaningRule{.id = "temp", .category = "temp", .targets = {"%TEMP%\\*"}}};
}

std::vector<map::Area> areas() {
  return {map::Area{.id = "cargo-home",
                    .paths = {LR"(%USERPROFILE%\.cargo)"},
                    .category = map::Category::dev_caches,
                    .label = "Cargo registry, git checkouts and installed tools"},
          map::Area{.id = "cargo-registry",
                    .paths = {LR"(%USERPROFILE%\.cargo\registry)"},
                    .category = map::Category::dev_caches,
                    .label = "Cargo downloaded crates"},
          map::Area{.id = "teams-cache",
                    .paths = {LR"(%USERPROFILE%\TeamsCache\*)"},
                    .category = map::Category::app_caches,
                    .label = "Teams caches"},
          // Documents are not kept out of the index.
          map::Area{.id = "documents",
                    .paths = {LR"(%USERPROFILE%\Documents)"},
                    .category = map::Category::user_documents,
                    .label = "Documents"}};
}

class IndexedFoldersTest : public ::testing::Test {
 protected:
  IndexedFoldersTest() {
    environment_.set(L"USERPROFILE", LR"(C:\Users\owner)");
    tree_.add_file(LR"(F:\app\package.json)", {}, {});
    tree_.add_directory(LR"(F:\app\node_modules\react)");
    tree_.add_directory(LR"(F:\app\dist)");
    tree_.add_directory(LR"(F:\app\src)");
    tree_.add_file(LR"(F:\rust\tool\Cargo.toml)", {}, {});
    tree_.add_file(LR"(F:\rust\tool\target\CACHEDIR.TAG)", {}, {});
    // A "target" folder without the tag is not Cargo's.
    tree_.add_file(LR"(F:\rust\other\Cargo.toml)", {}, {});
    tree_.add_directory(LR"(F:\rust\other\target)");
    // Below a skipped folder: never searched.
    tree_.add_file(LR"(F:\app\.git\hooks\package.json)", {}, {});
    tree_.add_directory(LR"(F:\app\.git\hooks\dist)");
    // Too deep for max_depth 3.
    tree_.add_file(LR"(F:\a\b\c\d\package.json)", {}, {});
    tree_.add_directory(LR"(F:\a\b\c\d\dist)");
    tree_.add_directory(LR"(C:\Users\owner\.cargo\registry)");
    tree_.add_directory(LR"(C:\Users\owner\TeamsCache\blob)");
    tree_.add_directory(LR"(C:\Users\owner\Documents)");
    // The owner added F:\ to the index; the profile is in Windows' default scope.
    scope_.rules_list = {
        ScopeRule{.url = LR"(file:///C:\Users\)", .included = true, .user = false},
        ScopeRule{.url = LR"(file:///F:\)", .included = true, .user = true},
        ScopeRule{.url = LR"(file:///F:\app\dist\)", .included = false, .user = true}};
  }

  [[nodiscard]] Result<std::vector<IndexedFolder>> find(const std::stop_token& stop = {}) const {
    const std::vector<map::Area> known_areas = areas();
    const std::vector<rules::CleaningRule> known_rules = project_rules();
    return indexed_build_folders(
        scope_, tree_, environment_,
        Knowledge{.areas = known_areas, .rules = known_rules, .workspaces = &workspaces_}, stop);
  }

  FakeScope& scope() { return scope_; }

 private:
  FakeTree tree_;
  FakeEnvironment environment_;
  FakeScope scope_;
  clean::Workspaces workspaces_{.roots = {LR"(F:\)"}, .skip = {L".git"}, .max_depth = 3};
};

TEST_F(IndexedFoldersTest, ListsTheIndexedBuildOutputAndCacheFolders) {
  const Result<std::vector<IndexedFolder>> found = find();
  ASSERT_TRUE(found.has_value()) << found.error().message;
  EXPECT_EQ(*found, (std::vector<IndexedFolder>{
                        {.path = LR"(C:\Users\owner\.cargo)",
                         .category = "dev-caches",
                         .label = "Cargo registry, git checkouts and installed tools"},
                        {.path = LR"(C:\Users\owner\TeamsCache\blob)",
                         .category = "app-caches",
                         .label = "Teams caches"},
                        {.path = LR"(F:\app\node_modules)",
                         .category = "build-artifacts",
                         .label = "Output of projects with package.json"},
                        {.path = LR"(F:\rust\tool\target)",
                         .category = "build-artifacts",
                         .label = "Output of projects with Cargo.toml"}}));
}

TEST_F(IndexedFoldersTest, AnIndexerThatDoesNotAnswerIsAnError) {
  scope().failure = Error{.code = Errc::io_error, .message = "Windows Search is not running"};
  const Result<std::vector<IndexedFolder>> found = find();
  ASSERT_FALSE(found.has_value());
  EXPECT_EQ(found.error().message, "Windows Search is not running");
}

TEST_F(IndexedFoldersTest, NothingIndexedNothingListed) {
  scope().rules_list.clear();
  EXPECT_TRUE(find().value().empty());
}

TEST_F(IndexedFoldersTest, AStoppedCheckFails) {
  std::stop_source stop;
  stop.request_stop();
  EXPECT_FALSE(find(stop.get_token()).has_value());
}

}  // namespace
}  // namespace myclean::indexer
