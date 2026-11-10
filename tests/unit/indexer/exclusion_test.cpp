#include "core/indexer/exclusion.hpp"

#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "core/indexer/scope.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "fake_scope.hpp"

namespace myclean::indexer {
namespace {

using testing::FakeScope;

const std::filesystem::path node_modules = LR"(F:\app\node_modules)";
const std::filesystem::path target = LR"(F:\app\target)";

TEST(FolderUrlTest, NamesTheFolderAndEverythingInIt) {
  EXPECT_EQ(folder_url(node_modules), LR"(file:///F:\app\node_modules\)");
  EXPECT_EQ(folder_url(LR"(F:\)"), LR"(file:///F:\)");
  const std::vector<ScopeRule> rules = {
      {.url = LR"(file:///F:\APP\TARGET\)", .included = true, .user = false},
      {.url = LR"(file:///F:\App\Target\)", .included = true, .user = true}};
  ASSERT_NE(user_rule(rules, folder_url(target)), nullptr);
  EXPECT_TRUE(user_rule(rules, folder_url(target))->user);
  EXPECT_EQ(user_rule(rules, folder_url(node_modules)), nullptr);
}

class IndexerExclusionTest : public ::testing::Test {
 protected:
  IndexerExclusionTest() {
    // F:\ is indexed because the owner added it; F:\app\target has a user rule of its own.
    scope_.rules_list = {
        ScopeRule{.url = LR"(file:///C:\Users\)", .included = true, .user = false},
        ScopeRule{.url = LR"(file:///F:\)", .included = true, .user = true},
        ScopeRule{.url = LR"(file:///F:\app\target\)", .included = true, .user = true}};
  }

  FakeScope& scope() { return scope_; }

 private:
  FakeScope scope_;
};

TEST_F(IndexerExclusionTest, DescribesAnAskTierServiceChange) {
  const IndexerExclusion action(scope(), {node_modules});
  const ActionInfo info = action.describe();
  EXPECT_EQ(info.tier, Tier::ask);
  EXPECT_EQ(info.context, ExecutionContext::service);
  EXPECT_EQ(info.risk, Risk::low);
}

TEST_F(IndexerExclusionTest, PreviewListsTheIndexedFoldersOnly) {
  IndexerExclusion action(scope(), {node_modules, target});
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->items[0].path, node_modules);
  EXPECT_EQ(preview->items[1].path, target);

  IndexerExclusion twice(scope(), {node_modules, node_modules});
  EXPECT_EQ(twice.preview().error().code, Errc::invalid_argument);
  IndexerExclusion outside(scope(), {LR"(D:\build)"});
  EXPECT_EQ(outside.preview().error().code, Errc::invalid_state);
  EXPECT_EQ(scope().writes, 0);
}

TEST_F(IndexerExclusionTest, ExcludesAndUndoesToTheExactScopeBefore) {
  const std::vector<ScopeRule> before = scope().rules_list;
  IndexerExclusion action(scope(), {node_modules, target});
  const Preview preview = action.preview().value();
  const Result<Undo> undo = action.apply(preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  ASSERT_EQ(undo->items.size(), 2U);
  EXPECT_FALSE(scope().included(node_modules).value());
  EXPECT_FALSE(scope().included(target).value());
  EXPECT_TRUE(scope().included(LR"(F:\app\src)").value());

  const Result<Verification> verified = action.verify(*undo);
  ASSERT_TRUE(verified.has_value()) << verified.error().message;
  EXPECT_EQ(verified->metric, "folders_out_of_index");
  EXPECT_EQ(verified->after, 2.0);
  EXPECT_EQ(verified->effect, Effect::better);

  ASSERT_TRUE(action.undo(*undo).has_value());
  // node_modules's rule is gone, target's own inclusion rule is back.
  std::vector<ScopeRule> after = scope().rules_list;
  const auto by_url = [](const ScopeRule& left, const ScopeRule& right) {
    return left.url < right.url;
  };
  std::vector<ScopeRule> expected = before;
  std::ranges::sort(after, by_url);
  std::ranges::sort(expected, by_url);
  EXPECT_EQ(after, expected);
  EXPECT_TRUE(scope().included(node_modules).value());
}

TEST_F(IndexerExclusionTest, UndoRefusesWhenTheRuleChangedMeanwhile) {
  IndexerExclusion action(scope(), {node_modules});
  const Undo undo = action.apply(action.preview().value()).value();
  ASSERT_TRUE(scope().set_rule(node_modules, true).has_value());
  const int writes = scope().writes;
  const Result<void> undone = action.undo(undo);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::invalid_state);
  EXPECT_EQ(scope().writes, writes);
  // The rule changed by someone else counts as not excluded.
  EXPECT_EQ(action.verify(undo).value().effect, Effect::worse);
}

TEST_F(IndexerExclusionTest, AFolderAlreadyOutIsSkippedAndAllOutIsAnError) {
  IndexerExclusion action(scope(), {node_modules, target});
  const Preview preview = action.preview().value();
  ASSERT_TRUE(scope().set_rule(target, false).has_value());
  const Undo undo = action.apply(preview).value();
  EXPECT_EQ(undo.items.size(), 1U);
  EXPECT_EQ(action.apply(preview).error().code, Errc::invalid_state);
}

TEST_F(IndexerExclusionTest, AFailedChangeReversesTheOnesMade) {
  // The fake fails every call once failure is set, so fail the second folder's lookup by
  // making the indexer go away after the first change: the first is reversed.
  class FailingSecond final : public IIndexerScope {
   public:
    explicit FailingSecond(FakeScope& inner) : inner_(&inner) {}
    [[nodiscard]] Result<std::vector<ScopeRule>> rules() const override { return inner_->rules(); }
    [[nodiscard]] Result<bool> included(const std::filesystem::path& folder) const override {
      return inner_->included(folder);
    }
    [[nodiscard]] Result<void> set_rule(const std::filesystem::path& folder,
                                        bool include) override {
      if (++sets_ == 2) {
        return fail(Errc::access_denied, "denied");
      }
      return inner_->set_rule(folder, include);
    }
    [[nodiscard]] Result<void> remove_rule(const std::filesystem::path& folder) override {
      return inner_->remove_rule(folder);
    }

   private:
    FakeScope* inner_;
    int sets_ = 0;
  };
  const std::vector<ScopeRule> before = scope().rules_list;
  FailingSecond failing(scope());
  IndexerExclusion action(failing, {node_modules, target});
  const Result<Undo> undo = action.apply(action.preview().value());
  ASSERT_FALSE(undo.has_value());
  EXPECT_EQ(undo.error().code, Errc::access_denied);
  EXPECT_EQ(scope().rules_list, before);
}

}  // namespace
}  // namespace myclean::indexer
