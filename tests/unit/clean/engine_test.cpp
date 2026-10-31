#include "core/clean/engine.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clean_fakes.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/catalog.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"
#include "fixture.hpp"

namespace myclean::clean {
namespace {

using namespace myclean::literals;
using namespace myclean::clean::testing;
namespace fs = std::filesystem;
using rules::CleaningRule;
using rules::Strategy;

const fs::path temp = R"(C:\Users\owner\AppData\Local\Temp)";
const fs::path chrome = R"(C:\Users\owner\AppData\Local\Google\Chrome\User Data)";

class RuleEngineTest : public ::testing::Test {
 protected:
  RuleEngineTest() { set_owner_environment(environment_); }

  [[nodiscard]] Evaluation evaluate(const CleaningRule& rule) const {
    return engine_.evaluate(std::span{&rule, 1}, now);
  }

  FakeTree& tree() { return tree_; }
  FakeUsage& usage() { return usage_; }
  [[nodiscard]] const RuleEngine& engine() const { return engine_; }

 private:
  FakeTree tree_;
  FakeEnvironment environment_;
  FakeUsage usage_;
  RuleEngine engine_{tree_, environment_, usage_, owner_locations()};
};

// Cleaning spec, "User temp rule": only files older than 2 days and not locked.
TEST_F(RuleEngineTest, UserTempSelectsOnlyOldUnlockedEntries) {
  tree().add_file(temp / "old.tmp", 10_KiB, days_ago(5));
  tree().add_file(temp / "new.tmp", 10_KiB, days_ago(1));
  tree().add_file(temp / "open.log", 10_KiB, days_ago(5));
  usage().lock(temp / "open.log");
  tree().add_directory(temp / "old_dir", days_ago(6));
  tree().add_file(temp / "old_dir" / "a.bin", 4_KiB, days_ago(6));
  tree().add_directory(temp / "mixed", days_ago(6));
  tree().add_file(temp / "mixed" / "a.bin", 4_KiB, days_ago(6));
  tree().add_file(temp / "mixed" / "b.bin", 4_KiB, days_ago(1));
  tree().add_directory(temp / "busy", days_ago(6));
  tree().add_file(temp / "busy" / "a.bin", 4_KiB, days_ago(6));
  usage().lock(temp / "busy" / "a.bin");

  const Evaluation evaluation = evaluate(shipped_rule("windows/temp-user"));

  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{temp / "old.tmp", temp / "old_dir"}));
  for (const Candidate& candidate : evaluation.candidates) {
    EXPECT_EQ(candidate.rule_id, "windows/temp-user");
    EXPECT_EQ(candidate.tier, Tier::automatic);
    EXPECT_EQ(candidate.scope, ExecutionContext::user);
    EXPECT_EQ(candidate.strategy, Strategy::remove);
    EXPECT_EQ(candidate.identity, tree().stat(candidate.path)->stat.identity);
  }
  EXPECT_EQ(evaluation.candidates.front().size, 10_KiB);
  ASSERT_NE(dropped_at(evaluation, temp / "open.log"), nullptr);
  EXPECT_EQ(dropped_at(evaluation, temp / "open.log")->reason, "in_use");
  ASSERT_NE(dropped_at(evaluation, temp / "busy"), nullptr);
  EXPECT_EQ(dropped_at(evaluation, temp / "busy")->reason, "in_use");
}

// Cleaning spec, "Browser rule limits".
TEST_F(RuleEngineTest, BrowserRuleSelectsOnlyHttpCodeAndGpuCaches) {
  for (const char* profile : {"Default", "Profile 1"}) {
    const fs::path root = chrome / profile;
    tree().add_file(root / "Cache" / "Cache_Data" / "data_0", 2_MiB, days_ago(1));
    tree().add_file(root / "Code Cache" / "js" / "index", 1_MiB, days_ago(1));
    tree().add_file(root / "GPUCache" / "data_1", 1_MiB, days_ago(1));
    tree().add_file(root / "Network" / "Cookies", 1_MiB, days_ago(1));
    tree().add_file(root / "History", 1_MiB, days_ago(1));
    tree().add_file(root / "Login Data", 1_MiB, days_ago(1));
    tree().add_file(root / "Sessions" / "Session_1", 1_MiB, days_ago(1));
    tree().add_file(root / "Preferences", 1_KiB, days_ago(1));
  }
  tree().add_file(chrome / "Local State", 1_KiB, days_ago(1));

  const Evaluation evaluation = evaluate(shipped_rule("browsers/chrome-cache"));

  EXPECT_EQ(paths_of(evaluation),
            (std::vector<fs::path>{chrome / "Default" / "Cache", chrome / "Default" / "Code Cache",
                                   chrome / "Default" / "GPUCache", chrome / "Profile 1" / "Cache",
                                   chrome / "Profile 1" / "Code Cache",
                                   chrome / "Profile 1" / "GPUCache"}));
  EXPECT_TRUE(evaluation.dropped.empty());
}

TEST_F(RuleEngineTest, RunningBrowserLeavesItsCacheAlone) {
  tree().add_file(chrome / "Default" / "Cache" / "Cache_Data" / "index", 1_MiB, days_ago(1));
  usage().lock(chrome / "Default" / "Cache" / "Cache_Data" / "index");

  const Evaluation evaluation = evaluate(shipped_rule("browsers/chrome-cache"));

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, chrome / "Default" / "Cache")->reason, "in_use");
}

// Cleaning spec, "Shader caches default to ask".
TEST_F(RuleEngineTest, ShaderCacheItemsAreAsk) {
  const fs::path dx_cache = R"(C:\Users\owner\AppData\Local\NVIDIA\DXCache)";
  tree().add_file(dx_cache / "shader.bin", 3_MiB, days_ago(10));

  const Evaluation evaluation = evaluate(shipped_rule("gpu/nvidia-dxcache"));

  ASSERT_EQ(evaluation.candidates.size(), 1U);
  EXPECT_EQ(evaluation.candidates.front().tier, Tier::ask);
  EXPECT_EQ(evaluation.candidates.front().path, dx_cache / "shader.bin");
}

// A candidate SafeRemove's deny-list would refuse is never proposed.
TEST_F(RuleEngineTest, DenyListedItemsAreDroppedWithTheEntryName) {
  tree().add_file(R"(C:\Windows\LiveKernelReports\WATCHDOG.dmp)", 1_GiB, days_ago(30));
  tree().add_file(R"(C:\Windows\Temp\setup.log)", 1_MiB, days_ago(30));
  tree().add_file(R"(C:\Program Files (x86)\Steam\appcache\httpcache\00\a)", 1_MiB, days_ago(30));

  for (const char* id :
       {"windows/live-kernel-reports", "windows/temp-system", "games/steam-httpcache"}) {
    SCOPED_TRACE(id);
    const Evaluation evaluation = evaluate(shipped_rule(id));
    if (std::string(id) == "windows/temp-system") {
      EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{R"(C:\Windows\Temp\setup.log)"}));
      EXPECT_EQ(evaluation.candidates.front().scope, ExecutionContext::service);
    } else {
      EXPECT_TRUE(evaluation.candidates.empty());
      ASSERT_EQ(evaluation.dropped.size(), 1U);
      EXPECT_TRUE(evaluation.dropped.front().reason.starts_with("deny_list:"));
    }
  }
}

TEST_F(RuleEngineTest, LinkInTheTargetPathIsDroppedAndNotEntered) {
  tree().add_directory(chrome / "Default", days_ago(1), {.reparse_point = true});
  tree().add_file(chrome / "Profile 1" / "Cache" / "f", 1_MiB, days_ago(1));
  tree().add_directory(chrome / "Profile 1" / "GPUCache", days_ago(1), {.reparse_point = true});

  const Evaluation evaluation = evaluate(shipped_rule("browsers/chrome-cache"));

  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{chrome / "Profile 1" / "Cache"}));
  EXPECT_EQ(dropped_at(evaluation, chrome / "Default")->reason, "reparse_point_in_path");
  EXPECT_EQ(tree().listed(chrome / "Default"), 0);
  EXPECT_EQ(tree().listed(chrome / "Profile 1" / "GPUCache"), 0);
}

TEST_F(RuleEngineTest, CloudPlaceholdersAndEmptyEntriesAreNotProposed) {
  tree().add_file(temp / "cloud.bin", 1_GiB, days_ago(10), {.recall_on_data_access = true});
  tree().add_directory(temp / "empty", days_ago(10));

  EXPECT_TRUE(evaluate(shipped_rule("windows/temp-user")).candidates.empty());
}

TEST_F(RuleEngineTest, UnreadableFolderIsDroppedWhenAgeOrUseMustBeProven) {
  tree().add_file(temp / "sealed" / "inner" / "a", 1_MiB, days_ago(10));
  tree().add_file(temp / "sealed" / "b", 1_MiB, days_ago(10));
  tree().deny(temp / "sealed" / "inner");

  const Evaluation evaluation = evaluate(shipped_rule("windows/temp-user"));

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, temp / "sealed")->reason, "unreadable");
}

// Cleaning spec, "Cache without a native command".
TEST_F(RuleEngineTest, GradleVersionFoldersOlderThanTheRuleAgeOnly) {
  const fs::path caches = R"(C:\Users\owner\.gradle\caches)";
  tree().add_directory(caches / "7.6", days_ago(200));
  tree().add_file(caches / "7.6" / "file-hashes.bin", 5_MiB, days_ago(200));
  tree().add_directory(caches / "8.5", days_ago(30));
  tree().add_file(caches / "8.5" / "file-hashes.bin", 5_MiB, days_ago(30));
  tree().add_file(caches / "modules-2" / "files-2.1" / "a.jar", 5_MiB, days_ago(200));

  const Evaluation evaluation = evaluate(shipped_rule("java-gradle/gradle-versions"));

  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{caches / "7.6"}));
}

TEST_F(RuleEngineTest, NativeRuleIsOneItemSizedByItsTargets) {
  const fs::path store = R"(C:\Users\owner\AppData\Local\pnpm\store)";
  tree().add_file(store / "v10" / "files" / "00" / "a", 2_MiB, days_ago(1));
  tree().add_file(store / "v10" / "files" / "01" / "b", 3_MiB, days_ago(1));

  const std::vector<CleaningRule> rules = {shipped_rule("node/pnpm-store"),
                                           shipped_rule("node/yarn-cache")};
  const Evaluation evaluation = engine().evaluate(rules, now);

  ASSERT_EQ(evaluation.candidates.size(), 1U);  // no Yarn cache on this machine
  const Candidate& pnpm = evaluation.candidates.front();
  EXPECT_EQ(pnpm.rule_id, "node/pnpm-store");
  EXPECT_EQ(pnpm.strategy, Strategy::native);
  EXPECT_EQ(pnpm.command, "pnpm store prune");
  EXPECT_EQ(pnpm.scope, ExecutionContext::user);
  EXPECT_TRUE(pnpm.path.empty());
  EXPECT_EQ(pnpm.size, 5_MiB);
}

TEST_F(RuleEngineTest, HandlerRuleIsAlwaysOneItem) {
  const Evaluation evaluation = evaluate(shipped_rule("windows/update-cleanup"));

  ASSERT_EQ(evaluation.candidates.size(), 1U);
  EXPECT_EQ(evaluation.candidates.front().strategy, Strategy::handler);
  EXPECT_EQ(evaluation.candidates.front().handler, "Update Cleanup");
  EXPECT_EQ(evaluation.candidates.front().scope, ExecutionContext::service);
  EXPECT_EQ(evaluation.candidates.front().tier, Tier::ask);
}

TEST_F(RuleEngineTest, AnEntryInsideAnotherCandidateIsProposedOnce) {
  tree().add_file(R"(C:\Cache\a\b\file)", 1_MiB, days_ago(10));
  tree().add_file(R"(C:\Cache a\file)", 1_MiB, days_ago(10));
  const CleaningRule outer{.id = "outer",
                           .category = "test",
                           .tier = Tier::automatic,
                           .scope = ExecutionContext::user,
                           .strategy = Strategy::remove,
                           .targets = {R"(C:\Cache\*)", R"(C:\Cache a)"}};
  const CleaningRule inner{.id = "inner",
                           .category = "test",
                           .tier = Tier::ask,
                           .scope = ExecutionContext::user,
                           .strategy = Strategy::remove,
                           .targets = {R"(C:\Cache\a\b)", R"(C:\Cache\a)"}};

  const std::vector<CleaningRule> rules = {outer, inner};
  const Evaluation evaluation = engine().evaluate(rules, now);

  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{R"(C:\Cache\a)", R"(C:\Cache a)"}));
  for (const Candidate& candidate : evaluation.candidates) {
    EXPECT_EQ(candidate.rule_id, "outer");
  }
  EXPECT_EQ(dropped_at(evaluation, R"(C:\Cache\a\b)")->reason, "covered_by:outer");
}

// Cleaning spec, "Invalid rule rejected": the file is rejected whole, so none of its targets
// reaches the engine.
TEST_F(RuleEngineTest, RejectedRuleFileSelectsNothing) {
  class TwoFiles final : public rules::IDataFiles {
   public:
    [[nodiscard]] Result<std::vector<fs::path>> list(const fs::path& /*dir*/) const override {
      return std::vector<fs::path>{R"(C:\data\rules\bad.toml)", R"(C:\data\rules\good.toml)"};
    }
    [[nodiscard]] Result<std::string> read(const fs::path& file) const override {
      const std::string strategy = file.stem() == "bad" ? "shred" : "delete";
      return "[[rule]]\nid = \"" + file.stem().string() +
             "\"\ncategory = \"test\"\ntier = \"auto\"\nscope = \"user\"\nstrategy = \"" +
             strategy + "\"\ntargets = ['C:\\" + file.stem().string() + "\\*']\n";
    }
  };
  tree().add_file(R"(C:\bad\a)", 1_MiB, days_ago(10));
  tree().add_file(R"(C:\good\a)", 1_MiB, days_ago(10));
  rules::RuleCatalog catalog = rules::make_rule_catalog(R"(C:\data)");

  const std::vector<Error> errors = catalog.reload(TwoFiles{});
  const Evaluation evaluation = engine().evaluate(catalog.entries(), now);

  ASSERT_EQ(errors.size(), 1U);
  EXPECT_NE(errors.front().message.find("'bad': field 'strategy'"), std::string::npos);
  EXPECT_EQ(paths_of(evaluation), (std::vector<fs::path>{R"(C:\good\a)"}));
}

TEST_F(RuleEngineTest, ProcessFolderInsideAnEntryMeansInUse) {
  tree().add_file(temp / "tool" / "tool.exe", 1_MiB, days_ago(10));
  usage().add_process_folder(temp / "tool");

  const Evaluation evaluation = evaluate(shipped_rule("windows/temp-user"));

  EXPECT_TRUE(evaluation.candidates.empty());
  EXPECT_EQ(dropped_at(evaluation, temp / "tool")->reason, "in_use");
}

}  // namespace
}  // namespace myclean::clean
