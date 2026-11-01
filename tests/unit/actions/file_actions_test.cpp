#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "actions_test_support.hpp"
#include "core/actions/audit_log.hpp"
#include "core/actions/delete_items.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::actions {
namespace {

using namespace myclean::literals;
using std::filesystem::path;
using testing::ActionsTest;
using Kind = fakes::FakeFileSystem::OperationKind;

class FileActionsTest : public ActionsTest {
 protected:
  // The entries of `plan` for `event`, as "<item> <outcome> <reason>".
  std::vector<std::string> outcomes(PlanId plan, const std::string& event) {
    std::vector<std::string> found;
    for (const LogEntry& entry : log(plan)) {
      if (entry.event == event) {
        found.push_back(std::to_string(entry.item.value_or(PlanItemId{}).value()) + " " +
                        entry.outcome + (entry.reason.empty() ? "" : " " + entry.reason));
      }
    }
    return found;
  }

  std::vector<QuarantineRecord> records(PlanId run) {
    std::vector<QuarantineRecord> found;
    EXPECT_TRUE(store()
                    .read([&](store::Database& db) -> Result<void> {
                      Result<std::vector<QuarantineRecord>> read = quarantine_records(db, run);
                      if (!read) {
                        return tl::unexpected(read.error());
                      }
                      found = *std::move(read);
                      return {};
                    })
                    .has_value());
    return found;
  }

  // An owner-approved plan quarantining F:\proj\big.log and F:\proj\out (a folder).
  Plan quarantined() {
    fs().add_file(L"F:\\proj\\big.log", 4_MiB);
    fs().add_file(L"F:\\proj\\out\\a.o", 1_MiB);
    const Plan plan = submit({L"F:\\proj\\big.log", L"F:\\proj\\out"}, AutomationLevel::suggest,
                             Actor::scheduler(), "QuarantineItems");
    const std::vector<PlanItemId> all = {plan.items[0].id, plan.items[1].id};
    EXPECT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
    QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner()};
    EXPECT_EQ(engine().execute(plan.id, action, Actor::owner(), false).value().state,
              PlanState::verified);
    return plan;
  }
};

TEST_F(FileActionsTest, SubsetApprovedOnlyThoseApplied) {
  std::vector<path> paths;
  for (int index = 1; index <= 5; ++index) {
    paths.emplace_back(L"F:\\cache\\" + std::to_wstring(index) + L".tmp");
    fs().add_file(paths.back(), 1_KiB);
  }
  const Plan plan = submit(paths, AutomationLevel::suggest, Actor::scheduler());
  const std::vector<PlanItemId> chosen = {plan.items[1].id, plan.items[3].id};
  ASSERT_EQ(engine().approve(plan.id, chosen, Actor::owner()).value(), PlanState::approved);

  DeleteItems action{environment(), plan.id, Actor::owner()};
  const RunReport report = engine().execute(plan.id, action, Actor::owner(), false).value();
  EXPECT_EQ(report.state, PlanState::verified);
  EXPECT_EQ(fs().operations(), (std::vector<fakes::FakeFileSystem::Operation>{
                                   {.kind = Kind::remove, .from = paths[1]},
                                   {.kind = Kind::remove, .from = paths[3]}}));
  for (const std::size_t index : {0U, 2U, 4U}) {
    EXPECT_TRUE(fs().exists(paths[index]));
    EXPECT_EQ(stored(plan.id).items[index].approval, ItemApproval::not_approved);
  }
  const auto id = [&](std::size_t index) { return std::to_string(plan.items[index].id.value()); };
  EXPECT_EQ(outcomes(plan.id, "approval"),
            (std::vector<std::string>{id(0) + " not_approved", id(1) + " approved",
                                      id(2) + " not_approved", id(3) + " approved",
                                      id(4) + " not_approved"}));
  EXPECT_EQ(outcomes(plan.id, "apply"),
            (std::vector<std::string>{id(1) + " removed", id(3) + " removed"}));
}

TEST_F(FileActionsTest, PreviewMatchesApply) {
  fs().add_file(L"F:\\cache\\a.bin", 400_MiB);
  fs().add_file(L"F:\\cache\\b.bin", 400_MiB);
  fs().add_file(L"F:\\cache\\c.bin", 400_MiB);
  fs().add_file(L"F:\\cache\\other.bin", 1_MiB);
  const Plan plan = submit({L"F:\\cache\\a.bin", L"F:\\cache\\b.bin", L"F:\\cache\\c.bin"},
                           AutomationLevel::automatic, Actor::scheduler());
  ASSERT_TRUE(engine().transition(plan.id, PlanState::applying, Actor::scheduler()).has_value());
  DeleteItems action{environment(), plan.id, Actor::scheduler()};
  const Preview preview = action.preview().value();
  ASSERT_EQ(preview.items.size(), 3U);
  EXPECT_EQ(preview.total_size(), 1200_MiB);

  const Undo undo = action.apply(preview).value();
  ASSERT_EQ(undo.items.size(), 3U);
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(undo.items[index].item, preview.items[index].id);
    EXPECT_FALSE(fs().exists(preview.items[index].path));
  }
  EXPECT_EQ(fs().operations().size(), 3U);
  EXPECT_TRUE(fs().exists(L"F:\\cache\\other.bin"));
}

TEST_F(FileActionsTest, PreviewThatDiffersFromThePlanIsRefused) {
  fs().add_file(L"F:\\cache\\a.bin", 1_KiB);
  fs().add_file(L"F:\\cache\\keep.bin", 1_KiB);
  const Plan plan = submit({L"F:\\cache\\a.bin"}, AutomationLevel::automatic, Actor::scheduler());
  ASSERT_TRUE(engine().transition(plan.id, PlanState::applying, Actor::scheduler()).has_value());
  DeleteItems action{environment(), plan.id, Actor::scheduler()};
  Preview preview = action.preview().value();
  preview.items[0].path = L"F:\\cache\\keep.bin";
  EXPECT_TRUE(action.apply(preview)->items.empty());
  EXPECT_TRUE(fs().operations().empty());
  EXPECT_EQ(outcomes(plan.id, "apply"),
            (std::vector<std::string>{std::to_string(plan.items[0].id.value()) +
                                      " refused preview_mismatch"}));
}

TEST_F(FileActionsTest, RawPathRefusedAndAudited) {
  fs().add_file(L"F:\\cache\\a.bin", 1_KiB);
  fs().add_file(L"F:\\work\\thesis.docx", 1_KiB);
  const Plan plan = submit({L"F:\\cache\\a.bin"}, AutomationLevel::automatic, Actor::scheduler());
  ASSERT_TRUE(engine().transition(plan.id, PlanState::applying, Actor::scheduler()).has_value());
  DeleteItems action{environment(), plan.id, Actor::scheduler()};
  // A request that carries a path but no stored plan item.
  const Preview raw{.items = {PlanItem{.path = L"F:\\work\\thesis.docx"}}};
  EXPECT_TRUE(action.apply(raw)->items.empty());
  EXPECT_TRUE(fs().operations().empty());
  const std::vector<LogEntry> entries = log(plan.id);
  const auto refusal =
      std::ranges::find_if(entries, [](const LogEntry& entry) { return entry.event == "apply"; });
  ASSERT_NE(refusal, entries.end());
  EXPECT_EQ(refusal->outcome, "refused");
  EXPECT_EQ(refusal->reason, "not_in_plan");
  EXPECT_TRUE(refusal->detail.starts_with("F:\\work\\thesis.docx")) << refusal->detail;
}

TEST_F(FileActionsTest, DeniedItemRefusedAndAuditedWithoutTouchingIt) {
  fs().add_file(L"C:\\Windows\\System32\\kernel32.dll", 1_MiB);
  fs().add_file(L"F:\\cache\\a.bin", 1_KiB);
  const Plan plan = submit({L"C:\\Windows\\System32\\kernel32.dll", L"F:\\cache\\a.bin"},
                           AutomationLevel::automatic, Actor::scheduler());
  DeleteItems action{environment(), plan.id, Actor::scheduler()};
  ASSERT_TRUE(engine().execute(plan.id, action, Actor::scheduler(), false).has_value());
  EXPECT_TRUE(fs().exists(L"C:\\Windows\\System32\\kernel32.dll"));
  EXPECT_EQ(fs().operations(), (std::vector<fakes::FakeFileSystem::Operation>{
                                   {.kind = Kind::remove, .from = L"F:\\cache\\a.bin"}}));
  EXPECT_EQ(outcomes(plan.id, "apply").front(),
            std::to_string(plan.items[0].id.value()) + " refused deny_list:windir");
}

TEST_F(FileActionsTest, DeleteDeclaresItIsNotReversible) {
  fs().add_file(L"F:\\cache\\a.bin", 1_KiB);
  const Plan plan = submit({L"F:\\cache\\a.bin"}, AutomationLevel::automatic, Actor::scheduler());
  DeleteItems action{environment(), plan.id, Actor::scheduler()};
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), true).value();
  EXPECT_EQ(report.state, PlanState::verified);
  EXPECT_FALSE(report.undo.reversible());
  const Result<void> undone = action.undo(report.undo);
  EXPECT_EQ(undone.error().code, Errc::not_reversible);
  EXPECT_EQ(undone.error().message, "not reversible: regenerates");
}

TEST_F(FileActionsTest, QuarantineMovesIntoTheVolumeFolderAndRecordsEachItem) {
  const FileIdentity log_identity = fs().add_file(L"F:\\proj\\big.log", 4_MiB);
  fs().add_file(L"F:\\proj\\out\\a.o", 1_MiB);
  const Plan plan = submit({L"F:\\proj\\big.log", L"F:\\proj\\out"}, AutomationLevel::suggest,
                           Actor::scheduler(), "QuarantineItems");
  const std::vector<PlanItemId> all = {plan.items[0].id, plan.items[1].id};
  ASSERT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner()};
  const RunReport report = engine().execute(plan.id, action, Actor::owner(), false).value();
  EXPECT_EQ(report.state, PlanState::verified);
  EXPECT_TRUE(report.undo.reversible());

  const path run = path{L"F:\\.myclean-quarantine"} / std::to_string(plan.id.value());
  const path log_location = run / std::to_string(plan.items[0].id.value()) / L"big.log";
  EXPECT_FALSE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_EQ(fs().stat(log_location)->identity, log_identity);
  EXPECT_TRUE(fs().exists(run / std::to_string(plan.items[1].id.value()) / L"out\\a.o"));
  EXPECT_FALSE(std::ranges::any_of(
      fs().operations(), [](const auto& operation) { return operation.kind == Kind::remove; }));

  const std::vector<QuarantineRecord> held = records(plan.id);
  ASSERT_EQ(held.size(), 2U);
  EXPECT_EQ(held[0].run, plan.id);
  EXPECT_EQ(held[0].original, path{L"F:\\proj\\big.log"});
  EXPECT_EQ(held[0].location, log_location);
  EXPECT_EQ(held[0].size, 4_MiB);
  EXPECT_EQ(held[0].identity, log_identity);
  EXPECT_EQ(held[0].moved_at, testing::noon);
  EXPECT_EQ(held[0].purge_after, testing::noon + std::chrono::days{7});
  EXPECT_EQ(held[0].state, QuarantineState::held);
}

TEST_F(FileActionsTest, ItemThatCouldNotMoveLeavesNoRecord) {
  fs().add_file(L"F:\\proj\\locked.log", 1_KiB);
  fs().fail_on(L"F:\\proj\\locked.log", Errc::in_use);
  const Plan plan = submit({L"F:\\proj\\locked.log"}, AutomationLevel::automatic,
                           Actor::scheduler(), "QuarantineItems");
  QuarantineItems action{environment(), quarantine(), plan.id, Actor::scheduler()};
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), false).value();
  EXPECT_TRUE(report.undo.items.empty());
  EXPECT_TRUE(records(plan.id).empty());
  EXPECT_EQ(
      outcomes(plan.id, "apply"),
      (std::vector<std::string>{std::to_string(plan.items[0].id.value()) + " failed in_use"}));
}

TEST_F(FileActionsTest, QuarantineNotPurgedBeforeExpiry) {
  const Plan plan = quarantined();
  clock().advance(std::chrono::days{6});
  EXPECT_EQ(quarantine().purge(Actor::scheduler()).value(), 0U);
  EXPECT_TRUE(fs().exists(records(plan.id)[0].location));
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::held);
}

TEST_F(FileActionsTest, QuarantinePurgedAfterExpiryThroughSafeRemove) {
  const Plan plan = quarantined();
  clock().advance(std::chrono::days{7} + std::chrono::seconds{1});
  EXPECT_EQ(quarantine().purge(Actor::scheduler()).value(), 2U);
  const path run = path{L"F:\\.myclean-quarantine"} / std::to_string(plan.id.value());
  EXPECT_FALSE(fs().exists(run));
  EXPECT_TRUE(fs().exists(L"F:\\.myclean-quarantine"));
  for (const QuarantineRecord& record : records(plan.id)) {
    EXPECT_EQ(record.state, QuarantineState::purged);
  }
  EXPECT_EQ(count("SELECT count(*) FROM plan WHERE action = 'PurgeQuarantine' "
                  "AND state = 'applied'"),
            1);
  EXPECT_EQ(quarantine().purge(Actor::scheduler()).value(), 0U);
}

TEST_F(FileActionsTest, TheOwnerPurgesOneRunBeforeItsExpiry) {
  const Plan first = quarantined();
  const Plan second = quarantined();
  clock().advance(std::chrono::days{1});
  EXPECT_EQ(quarantine().purge(Actor::scheduler(), first.id).error().code, Errc::refused);
  EXPECT_EQ(quarantine().purge(Actor::mcp("claude-code").value(), first.id).error().code,
            Errc::refused);
  EXPECT_EQ(records(first.id)[0].state, QuarantineState::held);

  EXPECT_EQ(quarantine().purge(Actor::owner(), first.id).value(), 2U);
  EXPECT_FALSE(fs().exists(path{L"F:\\.myclean-quarantine"} / std::to_string(first.id.value())));
  for (const QuarantineRecord& record : records(first.id)) {
    EXPECT_EQ(record.state, QuarantineState::purged);
  }
  // The other run keeps its items until its own expiry.
  EXPECT_TRUE(fs().exists(records(second.id)[0].location));
  EXPECT_EQ(records(second.id)[0].state, QuarantineState::held);
}

TEST_F(FileActionsTest, OnlyTheOwnerRollsBackAVerifiedPlan) {
  const Plan plan = quarantined();
  QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner()};
  EXPECT_EQ(engine().undo(plan.id, action, Actor::scheduler()).error().code, Errc::refused);
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::held);

  ASSERT_TRUE(engine().undo(plan.id, action, Actor::owner()).has_value());
  EXPECT_EQ(stored(plan.id).state, PlanState::rolled_back);
  EXPECT_TRUE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::restored);
}

TEST_F(FileActionsTest, OwnerUndoAfterRestartRestoresAppliedPlan) {
  fs().add_file(L"F:\\proj\\big.log", 4_MiB);
  const Plan plan = submit({L"F:\\proj\\big.log"}, AutomationLevel::suggest, Actor::scheduler(),
                           "QuarantineItems");
  const std::vector<PlanItemId> all = {plan.items[0].id};
  ASSERT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  {
    QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner()};
    ASSERT_TRUE(engine().transition(plan.id, PlanState::applying, Actor::owner()).has_value());
    const Undo undo = action.apply(action.preview().value()).value();
    ASSERT_TRUE(store()
                    .write([&](store::Database& db) -> Result<void> {
                      if (Result<void> saved = save_undo(db, 1, plan.id, undo); !saved) {
                        return saved;
                      }
                      return store::set_plan_state(db, plan.id, PlanState::applied);
                    })
                    .has_value());
  }
  ASSERT_FALSE(fs().exists(L"F:\\proj\\big.log"));

  reopen();  // the service restarts; only the stored undo data remains
  QuarantineItems action{environment(), quarantine(), plan.id, Actor::owner()};
  ASSERT_TRUE(engine().undo(plan.id, action, Actor::owner()).has_value());
  EXPECT_TRUE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_EQ(stored(plan.id).state, PlanState::rolled_back);
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::restored);
  const PlanId restore{count("SELECT id FROM plan WHERE action = 'RestoreQuarantine'")};
  EXPECT_EQ(outcomes(restore, "restore").size(), 1U);
  EXPECT_TRUE(outcomes(restore, "restore")[0].ends_with(" moved"));
  EXPECT_EQ(stored(restore).state, PlanState::applied);
}

TEST_F(FileActionsTest, RestoreOfARunReturnsEveryItem) {
  const Plan plan = quarantined();
  ASSERT_TRUE(quarantine().restore(plan.id, {}, Actor::owner()).has_value());
  EXPECT_TRUE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_TRUE(fs().exists(L"F:\\proj\\out\\a.o"));
  for (const QuarantineRecord& record : records(plan.id)) {
    EXPECT_EQ(record.state, QuarantineState::restored);
  }
}

TEST_F(FileActionsTest, RestoreRefusedForMcpActors) {
  const Plan plan = quarantined();
  EXPECT_EQ(quarantine().restore(plan.id, {}, Actor::mcp("claude-code").value()).error().code,
            Errc::refused);
  EXPECT_EQ(quarantine().purge(Actor::mcp("claude-code").value()).error().code, Errc::refused);
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::held);
}

TEST_F(FileActionsTest, RestoreRefusesAnItemSwappedInQuarantine) {
  const Plan plan = quarantined();
  const path location = records(plan.id)[0].location;
  fs().erase(location);
  fs().add_file(location, 1_KiB);  // someone replaced the quarantined file
  EXPECT_EQ(quarantine().restore(plan.id, {}, Actor::owner()).error().code, Errc::io_error);
  EXPECT_FALSE(fs().exists(L"F:\\proj\\big.log"));
  EXPECT_EQ(records(plan.id)[0].state, QuarantineState::held);
  EXPECT_EQ(records(plan.id)[1].state, QuarantineState::restored);
}

}  // namespace
}  // namespace myclean::actions
