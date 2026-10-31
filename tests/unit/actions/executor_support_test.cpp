// What the cleaning executor needs from the action framework: parameters stored with a plan,
// previews that carry the stored item ids, and quarantine on an overflow volume.

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "actions_test_support.hpp"
#include "core/actions/audit_log.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {
namespace {

using namespace myclean::literals;
using std::filesystem::path;

// Previews fresh items without ids, as the handler and setting actions do, and records what
// apply got.
class FreshPreviewAction final : public Action {
 public:
  explicit FreshPreviewAction(std::vector<path> paths) : paths_(std::move(paths)) {}

  [[nodiscard]] ActionInfo describe() const override { return {.what = "fresh"}; }
  [[nodiscard]] Result<Preview> preview() override {
    Preview preview;
    for (const path& item : paths_) {
      preview.items.push_back(PlanItem{.path = item, .size = 1_KiB});
    }
    return preview;
  }
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override {
    applied_ = preview.items;
    Undo undo{.not_reversible = "regenerates"};
    for (const PlanItem& item : preview.items) {
      undo.items.push_back({.item = item.id, .data = "x"});
    }
    return undo;
  }
  [[nodiscard]] Result<Verification> verify(const Undo& /*undo*/) override {
    return Verification{.metric = "m", .effect = Effect::better};
  }
  [[nodiscard]] Result<void> undo(const Undo& /*undo*/) override {
    return fail(Errc::not_reversible, "no");
  }

  [[nodiscard]] const std::vector<PlanItem>& applied() const { return applied_; }

 private:
  std::vector<path> paths_;
  std::vector<PlanItem> applied_;
};

class ExecutorSupportTest : public testing::ActionsTest {};

TEST_F(ExecutorSupportTest, ParamsAreStoredWithThePlan) {
  fs().add_file(L"F:\\cache\\a.tmp", 1_KiB);
  const Plan plan = engine()
                        .submit(plan_of({L"F:\\cache\\a.tmp"}, Actor::scheduler()),
                                AutomationLevel::suggest, R"({"mode":"reduced"})")
                        .value();
  const Plan other = submit({L"F:\\cache\\a.tmp"}, AutomationLevel::suggest, Actor::scheduler());
  std::string params;
  std::string none = "unset";
  ASSERT_TRUE(store()
                  .read([&](store::Database& db) -> Result<void> {
                    params = load_params(db, plan.id).value();
                    none = load_params(db, other.id).value();
                    return {};
                  })
                  .has_value());
  EXPECT_EQ(params, R"({"mode":"reduced"})");
  EXPECT_EQ(none, "");
}

TEST_F(ExecutorSupportTest, PreviewItemsCarryStoredIdsAndRejectedOnesAreDropped) {
  Plan plan{
      .action = "HandlerCleanup", .proposer = Actor::scheduler(), .created_at = clock().now()};
  for (const wchar_t* item : {L"handler:Update Cleanup", L"handler:Setup Log Files"}) {
    plan.items.push_back(PlanItem{.path = item, .size = 1_KiB});
  }
  const Plan stored_plan = engine().submit(plan, AutomationLevel::suggest).value();
  const std::vector<PlanItemId> first = {stored_plan.items[0].id};
  ASSERT_EQ(engine().approve(stored_plan.id, first, Actor::owner()).value(), PlanState::approved);

  FreshPreviewAction action{
      {L"handler:Update Cleanup", L"handler:Setup Log Files", L"handler:Never Shown"}};
  const RunReport report = engine().execute(stored_plan.id, action, Actor::owner(), false).value();
  EXPECT_EQ(report.state, PlanState::verified);
  ASSERT_EQ(action.applied().size(), 1U);
  EXPECT_EQ(action.applied()[0].id, stored_plan.items[0].id);
  ASSERT_EQ(report.undo.items.size(), 1U);
  EXPECT_EQ(report.undo.items[0].item, stored_plan.items[0].id);
}

TEST_F(ExecutorSupportTest, OverflowQuarantineMovesFilesToAnotherVolumeAndRestoresThem) {
  fs().add_file(L"C:\\Temp\\big.iso", 4_MiB);
  fs().add_directory(L"L:\\data");
  const Plan plan = submit({L"C:\\Temp\\big.iso"}, AutomationLevel::suggest, Actor::scheduler(),
                           "QuarantineItems");
  const std::vector<PlanItemId> all = {plan.items[0].id};
  ASSERT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  QuarantineItems action{
      environment(), quarantine(), plan.id, Actor::owner(),
      [](const PlanItem&) -> tl::expected<path, std::string> { return L"L:\\"; }};
  const RunReport report = engine().execute(plan.id, action, Actor::owner(), false).value();
  EXPECT_EQ(report.state, PlanState::verified);
  const path location = L"L:\\.myclean-quarantine\\" + std::to_wstring(plan.id.value()) + L"\\" +
                        std::to_wstring(plan.items[0].id.value()) + L"\\big.iso";
  EXPECT_TRUE(fs().exists(location));
  EXPECT_FALSE(fs().exists(L"C:\\Temp\\big.iso"));

  // The record holds the identity the file has on L:, so the restore passes SafeRemove.
  ASSERT_TRUE(quarantine().restore(plan.id, {}, Actor::owner()).has_value());
  EXPECT_TRUE(fs().exists(L"C:\\Temp\\big.iso"));
  EXPECT_FALSE(fs().exists(location));
}

TEST_F(ExecutorSupportTest, ItemsWithoutAQuarantineRootAreSkippedWithTheReason) {
  fs().add_file(L"C:\\Temp\\big.iso", 4_MiB);
  const Plan plan = submit({L"C:\\Temp\\big.iso"}, AutomationLevel::suggest, Actor::scheduler(),
                           "QuarantineItems");
  const std::vector<PlanItemId> all = {plan.items[0].id};
  ASSERT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner(),
                         [](const PlanItem&) -> tl::expected<path, std::string> {
                           return tl::unexpected<std::string>("quarantine volume health degraded");
                         }};
  ASSERT_TRUE(engine().execute(plan.id, action, Actor::owner(), false).has_value());
  EXPECT_TRUE(fs().exists(L"C:\\Temp\\big.iso"));
  bool logged = false;
  for (const LogEntry& entry : log(plan.id)) {
    logged = logged || (entry.event == "apply" && entry.outcome == "skipped" &&
                        entry.reason == "quarantine volume health degraded");
  }
  EXPECT_TRUE(logged);
}

TEST_F(ExecutorSupportTest, PurgeRemovesRunFoldersBelowAnyRoot) {
  fs().add_file(L"C:\\sandbox\\items\\a.log", 1_KiB);
  const Plan plan = submit({L"C:\\sandbox\\items\\a.log"}, AutomationLevel::suggest,
                           Actor::scheduler(), "QuarantineItems");
  const std::vector<PlanItemId> all = {plan.items[0].id};
  ASSERT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  QuarantineItems action{
      environment(), quarantine(), plan.id, Actor::owner(),
      [](const PlanItem&) -> tl::expected<path, std::string> { return L"C:\\sandbox\\q"; }};
  ASSERT_TRUE(engine().execute(plan.id, action, Actor::owner(), false).has_value());
  const path run = L"C:\\sandbox\\q\\.myclean-quarantine\\" + std::to_wstring(plan.id.value());
  ASSERT_TRUE(fs().exists(run));

  clock().advance(Quarantine::hold_time + std::chrono::hours{1});
  EXPECT_EQ(quarantine().purge(Actor::scheduler()).value(), 1U);
  EXPECT_FALSE(fs().exists(run));
}

}  // namespace
}  // namespace myclean::actions
