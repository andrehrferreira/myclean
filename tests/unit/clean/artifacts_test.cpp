#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clean_fakes.hpp"
#include "core/clean/engine.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/action.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "fixture.hpp"

namespace myclean::clean {
namespace {

using namespace myclean::literals;
using namespace myclean::clean::testing;
namespace fs = std::filesystem;
using rules::CleaningRule;

class ArtifactScanTest : public ::testing::Test {
 protected:
  [[nodiscard]] Evaluation scan() const {
    return engine_.scan_artifacts(shipped_rules(), workspaces_, git_, now);
  }

  // A git project at `root` with `marker`, one source file and a commit, all `age` days old.
  void add_project(const fs::path& root, const char* marker, int age) {
    tree_.add_directory(root, days_ago(age));
    tree_.add_directory(root / ".git", days_ago(age));
    tree_.add_file(root / ".git" / "HEAD", 1_KiB, days_ago(age));
    tree_.add_file(root / marker, 1_KiB, days_ago(age));
    tree_.add_file(root / "src" / "main.src", 1_KiB, days_ago(age));
    git_.set_last_commit(root, days_ago(age));
  }

  FakeTree& tree() { return tree_; }
  FakeUsage& usage() { return usage_; }
  FakeGit& git() { return git_; }
  Workspaces& workspaces() { return workspaces_; }
  [[nodiscard]] const RuleEngine& engine() const { return engine_; }

 private:
  FakeTree tree_;
  FakeEnvironment environment_;
  FakeUsage usage_;
  FakeGit git_;
  Workspaces workspaces_{.roots = {R"(F:\)"}, .skip = {L".git", L"node_modules"}, .max_depth = 8};
  RuleEngine engine_{tree_, environment_, usage_, owner_locations()};
};

// Cleaning spec, "Ignored artifact of an inactive project".
TEST_F(ArtifactScanTest, IgnoredArtifactOfAnInactiveProjectIsProposed) {
  add_project(R"(F:\app)", "package.json", 45);
  tree().add_file(R"(F:\app\node_modules\left-pad\index.js)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\app\node_modules)");

  const Evaluation evaluation = scan();

  ASSERT_EQ(evaluation.candidates.size(), 1U);
  const Candidate& candidate = evaluation.candidates.front();
  EXPECT_EQ(candidate.path, fs::path{R"(F:\app\node_modules)"});
  EXPECT_EQ(candidate.rule_id, "dev.node");
  EXPECT_EQ(candidate.size, 1_MiB);
  EXPECT_EQ(candidate.project, fs::path{R"(F:\app)"});
  EXPECT_EQ(candidate.tier, Tier::automatic);
  EXPECT_TRUE(evaluation.dropped.empty());
}

// Cleaning spec, "Tracked folder never touched".
TEST_F(ArtifactScanTest, FolderGitDoesNotIgnoreOrThatHoldsTrackedFilesIsNotProposed) {
  add_project(R"(F:\lib)", "package.json", 45);
  tree().add_file(R"(F:\lib\dist\index.js)", 1_MiB, days_ago(45));
  tree().add_file(R"(F:\lib\build\keep.txt)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\lib\build)");
  git().track(R"(F:\lib\build)");

  const Evaluation evaluation = scan();

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, R"(F:\lib\dist)")->reason, "not_ignored");
  EXPECT_EQ(dropped_at(evaluation, R"(F:\lib\build)")->reason, "tracked_files");
}

// Cleaning spec, "Build running in the folder".
TEST_F(ArtifactScanTest, OpenFileOrProcessInTheProjectKeepsItsArtifacts) {
  add_project(R"(F:\game)", "Cargo.toml", 45);
  tree().add_file(R"(F:\game\target\debug\game.pdb)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\game\target)");
  usage().lock(R"(F:\game\target\debug\game.pdb)");
  add_project(R"(F:\tool)", "Cargo.toml", 45);
  tree().add_file(R"(F:\tool\target\release\tool.exe)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\tool\target)");
  usage().add_process_folder(R"(F:\tool\src)");  // a shell or compiler working in the project

  const Evaluation evaluation = scan();

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, R"(F:\game\target)")->reason, "in_use");
  EXPECT_EQ(dropped_at(evaluation, R"(F:\tool\target)")->reason, "in_use");
}

// Cleaning spec, "Active project".
TEST_F(ArtifactScanTest, RecentSourceChangeOrCommitKeepsTheProjectActive) {
  add_project(R"(F:\edited)", "package.json", 45);
  tree().add_file(R"(F:\edited\src\new.ts)", 1_KiB, days_ago(3));
  tree().add_file(R"(F:\edited\node_modules\x\index.js)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\edited\node_modules)");
  add_project(R"(F:\committed)", "package.json", 45);
  git().set_last_commit(R"(F:\committed)", days_ago(3));
  tree().add_file(R"(F:\committed\node_modules\x\index.js)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\committed\node_modules)");

  const Evaluation evaluation = scan();

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_TRUE(evaluation.dropped.empty());
}

TEST_F(ArtifactScanTest, ArtifactChangesDoNotCountAsActivity) {
  add_project(R"(F:\app)", "package.json", 45);
  tree().add_file(R"(F:\app\node_modules\.cache\fresh)", 1_MiB, days_ago(1));
  git().ignore(R"(F:\app\node_modules)");

  EXPECT_EQ(paths_of(scan()), (std::vector<fs::path>{R"(F:\app\node_modules)"}));
}

// Cleaning spec, "Unreal Saved folder".
TEST_F(ArtifactScanTest, UnrealSavedLogsAndCrashesOnlyNeverSavedItself) {
  add_project(R"(F:\ue)", "Shooter.uproject", 45);
  for (const char* folder : {"Logs", "Crashes", "Config"}) {
    const fs::path path = fs::path{R"(F:\ue\Saved)"} / folder;
    tree().add_file(path / "file", 1_MiB, days_ago(45));
    git().ignore(path);
  }
  git().ignore(R"(F:\ue\Saved)");

  const Evaluation evaluation = scan();

  EXPECT_EQ(paths_of(evaluation),
            (std::vector<fs::path>{R"(F:\ue\Saved\Crashes)", R"(F:\ue\Saved\Logs)"}));
  for (const Candidate& candidate : evaluation.candidates) {
    EXPECT_EQ(candidate.tier, Tier::ask);
  }
}

TEST_F(ArtifactScanTest, UnityProjectIsFoundByARelativeMarker) {
  add_project(R"(F:\unity)", R"(ProjectSettings\ProjectVersion.txt)", 45);
  tree().add_file(R"(F:\unity\Library\ArtifactDB)", 2_MiB, days_ago(45));
  git().ignore(R"(F:\unity\Library)");

  EXPECT_EQ(paths_of(scan()), (std::vector<fs::path>{R"(F:\unity\Library)"}));
}

TEST_F(ArtifactScanTest, NeverEntersSkippedOrArtifactFolders) {
  add_project(R"(F:\app)", "package.json", 45);
  tree().add_file(R"(F:\app\node_modules\dep\package.json)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\app\node_modules\dep\dist\index.js)", 1_MiB, days_ago(45));
  tree().add_file(R"(F:\app\dist\sub\package.json)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\app\dist\sub\build\x)", 1_KiB, days_ago(45));
  git().ignore(R"(F:\app\node_modules)");
  git().ignore(R"(F:\app\dist)");

  const Evaluation evaluation = scan();

  EXPECT_EQ(paths_of(evaluation),
            (std::vector<fs::path>{R"(F:\app\dist)", R"(F:\app\node_modules)"}));
  EXPECT_EQ(tree().listed(R"(F:\app\.git)"), 0);
}

// Outside git nothing vouches that a folder is regenerable: an engine's ThirdParty "build" or a
// tool's "bin" looks like an artifact too (seen in the read-only smoke run on this PC).
TEST_F(ArtifactScanTest, WithoutGitNothingIsProposed) {
  tree().add_file(R"(F:\loose\Cargo.toml)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\loose\target\debug\a.exe)", 1_MiB, days_ago(45));

  const Evaluation evaluation = scan();

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, R"(F:\loose\target)")->reason, "not_in_git");
}

TEST_F(ArtifactScanTest, GitFailureProposesNothingFromThatRepository) {
  add_project(R"(F:\app)", "package.json", 45);
  tree().add_file(R"(F:\app\node_modules\x\index.js)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\app\node_modules)");
  git().fail_in(R"(F:\app)");

  const Evaluation evaluation = scan();

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, R"(F:\app\node_modules)")->reason, "git_error");
}

TEST_F(ArtifactScanTest, ProjectInsideAMonorepoUsesTheEnclosingWorkTree) {
  add_project(R"(F:\mono)", "package.json", 45);
  tree().add_file(R"(F:\mono\packages\web\package.json)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\mono\packages\web\.next\cache\x)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\mono\packages\web\.next)");

  EXPECT_EQ(paths_of(scan()), (std::vector<fs::path>{R"(F:\mono\packages\web\.next)"}));
}

TEST_F(ArtifactScanTest, ArtifactSelectedByTwoRulesIsProposedOnce) {
  add_project(R"(F:\native)", "package.json", 45);
  tree().add_file(R"(F:\native\CMakeLists.txt)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\native\build\a.obj)", 1_MiB, days_ago(45));
  git().ignore(R"(F:\native\build)");

  const Evaluation evaluation = scan();

  ASSERT_EQ(evaluation.candidates.size(), 1U);
  EXPECT_EQ(evaluation.candidates.front().rule_id, "dev.node");
  EXPECT_EQ(dropped_at(evaluation, R"(F:\native\build)")->reason, "covered_by:dev.node");
}

TEST_F(ArtifactScanTest, DefaultInactiveDaysAre30ForAutoAnd14Otherwise) {
  const CleaningRule automatic{.id = "auto",
                               .category = "test",
                               .tier = Tier::automatic,
                               .scope = ExecutionContext::user,
                               .strategy = rules::Strategy::remove,
                               .targets = {"out_auto"},
                               .project_marker = "marker"};
  CleaningRule ask = automatic;
  ask.id = "ask";
  ask.tier = Tier::ask;
  ask.targets = {"out_ask"};
  add_project(R"(F:\p)", "marker", 20);
  tree().add_file(R"(F:\p\out_auto\x)", 1_MiB, days_ago(20));
  tree().add_file(R"(F:\p\out_ask\x)", 1_MiB, days_ago(20));

  const std::vector<CleaningRule> rules = {automatic, ask};
  const Evaluation evaluation = engine().scan_artifacts(rules, workspaces(), git(), now);

  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{R"(F:\p\out_ask)"}));
}

TEST_F(ArtifactScanTest, MaxDepthBoundsTheSearch) {
  workspaces().max_depth = 2;
  tree().add_file(R"(F:\a\b\c\package.json)", 1_KiB, days_ago(45));
  tree().add_file(R"(F:\a\b\c\dist\x)", 1_MiB, days_ago(45));

  EXPECT_TRUE(scan().candidates.empty());
  EXPECT_EQ(tree().listed(R"(F:\a\b\c)"), 0);
}

}  // namespace
}  // namespace myclean::clean
