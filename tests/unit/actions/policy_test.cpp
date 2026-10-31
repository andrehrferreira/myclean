#include "core/actions/policy.hpp"

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "actions_test_support.hpp"
#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"

namespace myclean::actions {
namespace {

using namespace myclean::literals;
using testing::ActionsTest;

constexpr std::array<PlanState, 10> all_states = {
    PlanState::proposed, PlanState::automatic, PlanState::awaiting_approval, PlanState::approved,
    PlanState::applying, PlanState::applied,   PlanState::verified,          PlanState::rolled_back,
    PlanState::failed,   PlanState::rejected,
};

const Actor mcp_client = Actor::mcp("claude-code").value();

// An action whose outcome the test decides.
class ScriptedAction final : public Action {
 public:
  ScriptedAction() = default;

  [[nodiscard]] ActionInfo describe() const override { return {.what = "scripted"}; }
  [[nodiscard]] Result<Preview> preview() override { return Preview{}; }
  [[nodiscard]] Result<Undo> apply(const Preview& /*preview*/) override {
    if (apply_fails) {
      return fail(Errc::io_error, "disk gone");
    }
    return Undo{.items = {{.item = PlanItemId{1}, .data = "payload"}},
                .not_reversible = reversible ? "" : "regenerates"};
  }
  [[nodiscard]] Result<Verification> verify(const Undo& /*undo*/) override {
    return Verification{.metric = "volume.free", .before = 1.0, .after = 2.0, .effect = effect};
  }
  [[nodiscard]] Result<void> undo(const Undo& undo) override {
    undone.push_back(undo.items.size());
    return {};
  }

  bool apply_fails = false;
  bool reversible = true;
  Effect effect = Effect::better;
  std::vector<std::size_t> undone;  // the item count of each undo call
};

class PolicyTest : public ActionsTest {
 protected:
  Plan suggest(const Actor& proposer = Actor::scheduler()) {
    fs().add_file(L"F:\\cache\\" + std::to_wstring(++files_) + L".tmp", 1_KiB);
    return submit({L"F:\\cache\\" + std::to_wstring(files_) + L".tmp"}, AutomationLevel::suggest,
                  proposer);
  }

  Plan automatic() {
    fs().add_file(L"F:\\cache\\" + std::to_wstring(++files_) + L".tmp", 1_KiB);
    return submit({L"F:\\cache\\" + std::to_wstring(files_) + L".tmp"}, AutomationLevel::automatic,
                  Actor::scheduler());
  }

  // The (to, outcome, actor) of every transition entry of `plan`.
  std::vector<std::string> transitions(PlanId plan) {
    std::vector<std::string> found;
    for (const LogEntry& entry : log(plan)) {
      if (entry.event == "transition") {
        found.push_back(std::string(to_string(*entry.to)) + " " + entry.outcome + " " +
                        entry.actor);
      }
    }
    return found;
  }

 private:
  int files_ = 0;
};

TEST(PlanStateMachine, OnlyTheSpecifiedTransitionsAreLegal) {
  const std::set<std::pair<PlanState, PlanState>> legal = {
      {PlanState::proposed, PlanState::automatic},
      {PlanState::proposed, PlanState::awaiting_approval},
      {PlanState::automatic, PlanState::approved},
      {PlanState::awaiting_approval, PlanState::approved},
      {PlanState::awaiting_approval, PlanState::rejected},
      {PlanState::approved, PlanState::applying},
      {PlanState::applying, PlanState::applied},
      {PlanState::applying, PlanState::failed},
      {PlanState::applied, PlanState::verified},
      {PlanState::applied, PlanState::rolled_back},
      {PlanState::applied, PlanState::failed},
      {PlanState::verified, PlanState::rolled_back},
  };
  for (const PlanState from : all_states) {
    for (const PlanState to : all_states) {
      EXPECT_EQ(is_legal(from, to), legal.contains({from, to}))
          << to_string(from) << " -> " << to_string(to);
    }
  }
}

TEST(PlanStateMachine, McpActorsCanNeverCauseApproval) {
  for (const PlanState from : all_states) {
    EXPECT_FALSE(may_request(mcp_client, from, PlanState::approved));
    EXPECT_FALSE(may_request(mcp_client, from, PlanState::automatic));
  }
  EXPECT_TRUE(may_request(mcp_client, PlanState::proposed, PlanState::awaiting_approval));
  EXPECT_FALSE(may_request(Actor::scheduler(), PlanState::awaiting_approval, PlanState::approved));
  EXPECT_TRUE(may_request(Actor::owner(), PlanState::awaiting_approval, PlanState::approved));
  EXPECT_TRUE(may_request(Actor::scheduler(), PlanState::automatic, PlanState::approved));
}

TEST_F(PolicyTest, AutoFeatureSkipsApproval) {
  const Plan plan = automatic();
  EXPECT_EQ(plan.state, PlanState::approved);
  ScriptedAction action;
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), false).value();
  EXPECT_EQ(report.state, PlanState::verified);
  EXPECT_EQ(
      transitions(plan.id),
      (std::vector<std::string>{"proposed accepted scheduler", "auto accepted scheduler",
                                "approved accepted scheduler", "applying accepted scheduler",
                                "applied accepted scheduler", "verified accepted scheduler"}));
}

TEST_F(PolicyTest, SuggestFeatureWaitsForApproval) {
  const Plan plan = suggest();
  EXPECT_EQ(plan.state, PlanState::awaiting_approval);
  EXPECT_EQ(stored(plan.id).state, PlanState::awaiting_approval);
  ScriptedAction action;
  EXPECT_EQ(engine().execute(plan.id, action, Actor::scheduler(), false).error().code,
            Errc::invalid_state);
  EXPECT_EQ(stored(plan.id).state, PlanState::awaiting_approval);
}

TEST_F(PolicyTest, ObserveFeatureNeverCreatesAPlan) {
  fs().add_file(L"F:\\cache\\a.tmp", 1_KiB);
  EXPECT_EQ(
      engine()
          .submit(plan_of({L"F:\\cache\\a.tmp"}, Actor::scheduler()), AutomationLevel::observe)
          .error()
          .code,
      Errc::refused);
  EXPECT_EQ(count("SELECT count(*) FROM plan"), 0);
}

TEST_F(PolicyTest, IllegalTransitionRejectedAndLogged) {
  const Plan plan = suggest();
  for (const Actor& actor : {Actor::scheduler(), Actor::owner()}) {
    EXPECT_EQ(engine().transition(plan.id, PlanState::applying, actor).error().code,
              Errc::invalid_state);
  }
  EXPECT_EQ(stored(plan.id).state, PlanState::awaiting_approval);
  const LogEntry last = log(plan.id).back();
  EXPECT_EQ(last.outcome, "rejected");
  EXPECT_EQ(last.from, PlanState::awaiting_approval);
  EXPECT_EQ(last.to, PlanState::applying);
}

TEST_F(PolicyTest, McpActorProposalWaitsForTheOwner) {
  fs().add_file(L"F:\\cache\\a.tmp", 1_KiB);
  const Plan plan = submit({L"F:\\cache\\a.tmp"}, AutomationLevel::automatic, mcp_client);
  EXPECT_EQ(plan.state, PlanState::awaiting_approval);
  EXPECT_EQ(stored(plan.id).proposer, mcp_client);
  EXPECT_EQ(transitions(plan.id),
            (std::vector<std::string>{"proposed accepted mcp:claude-code",
                                      "awaiting_approval accepted mcp:claude-code"}));
}

TEST_F(PolicyTest, McpActorCannotApprove) {
  const Plan plan = suggest(mcp_client);
  const std::vector<PlanItemId> all = {plan.items.at(0).id};
  EXPECT_EQ(engine().approve(plan.id, all, mcp_client).error().code, Errc::refused);
  EXPECT_EQ(engine().transition(plan.id, PlanState::approved, mcp_client).error().code,
            Errc::refused);
  EXPECT_EQ(stored(plan.id).state, PlanState::awaiting_approval);
  EXPECT_EQ(stored(plan.id).items.at(0).approval, ItemApproval::pending);
  const std::vector<std::string> logged = transitions(plan.id);
  EXPECT_EQ(std::vector<std::string>(logged.end() - 2, logged.end()),
            (std::vector<std::string>{"approved rejected mcp:claude-code",
                                      "approved rejected mcp:claude-code"}));
}

TEST_F(PolicyTest, McpActorCannotApproveThroughAuto) {
  fs().add_file(L"F:\\cache\\a.tmp", 1_KiB);
  Plan plan = plan_of({L"F:\\cache\\a.tmp"}, Actor::scheduler());
  ASSERT_TRUE(store()
                  .write([&](store::Database& db) -> Result<void> {
                    Result<Plan> added = store::add_plan(db, plan);
                    if (!added) {
                      return tl::unexpected(added.error());
                    }
                    plan = *std::move(added);
                    return {};
                  })
                  .has_value());
  EXPECT_EQ(engine().transition(plan.id, PlanState::automatic, mcp_client).error().code,
            Errc::refused);
  ASSERT_TRUE(engine().transition(plan.id, PlanState::automatic, Actor::scheduler()).has_value());
  EXPECT_EQ(engine().transition(plan.id, PlanState::approved, mcp_client).error().code,
            Errc::refused);
  EXPECT_EQ(stored(plan.id).state, PlanState::automatic);
}

TEST_F(PolicyTest, OnlyTheOwnerApproves) {
  const Plan plan = suggest();
  const std::vector<PlanItemId> all = {plan.items.at(0).id};
  EXPECT_EQ(engine().approve(plan.id, all, Actor::scheduler()).error().code, Errc::refused);
  EXPECT_EQ(engine().approve(plan.id, all, Actor::governor()).error().code, Errc::refused);
  EXPECT_EQ(engine().approve(plan.id, all, Actor::owner()).value(), PlanState::approved);
  EXPECT_EQ(stored(plan.id).state, PlanState::approved);
  const std::vector<std::string> logged = transitions(plan.id);
  EXPECT_EQ(logged.front(), "proposed accepted scheduler");
  EXPECT_EQ(logged.back(), "approved accepted owner");
}

TEST_F(PolicyTest, ApprovingNoneRejectsThePlan) {
  const Plan plan = suggest();
  EXPECT_EQ(engine().approve(plan.id, {}, Actor::owner()).value(), PlanState::rejected);
  EXPECT_EQ(stored(plan.id).state, PlanState::rejected);
  EXPECT_EQ(stored(plan.id).items.at(0).approval, ItemApproval::not_approved);
  EXPECT_EQ(engine().transition(plan.id, PlanState::approved, Actor::owner()).error().code,
            Errc::invalid_state);
}

TEST_F(PolicyTest, ApprovingAnItemOutsideThePlanChangesNothing) {
  const Plan plan = suggest();
  const std::vector<PlanItemId> wrong = {PlanItemId{999}};
  EXPECT_EQ(engine().approve(plan.id, wrong, Actor::owner()).error().code, Errc::invalid_argument);
  EXPECT_EQ(stored(plan.id).state, PlanState::awaiting_approval);
  EXPECT_EQ(transitions(plan.id).size(), 2U);
}

TEST_F(PolicyTest, UndoDataIsStoredWithTheAppliedTransition) {
  const Plan plan = automatic();
  ScriptedAction action;
  ASSERT_TRUE(engine().execute(plan.id, action, Actor::scheduler(), false).has_value());
  EXPECT_EQ(count("SELECT count(*) FROM undo_blob u JOIN action_log l ON l.id = u.log_id "
                  "WHERE l.to_state = 'applied' AND u.plan_id = " +
                  std::to_string(plan.id.value())),
            2);  // the plan-wide row and one item
}

TEST_F(PolicyTest, WorseResultRolledBackByPolicy) {
  const Plan plan = automatic();
  ScriptedAction action;
  action.effect = Effect::worse;
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), true).value();
  EXPECT_EQ(report.state, PlanState::rolled_back);
  EXPECT_FALSE(report.alert);
  EXPECT_EQ(action.undone, (std::vector<std::size_t>{1}));
  EXPECT_EQ(stored(plan.id).state, PlanState::rolled_back);
}

TEST_F(PolicyTest, WorseResultWithoutAutoUndoAlertsAndOffersUndo) {
  const Plan plan = automatic();
  ScriptedAction action;
  action.effect = Effect::worse;
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), false).value();
  EXPECT_EQ(report.state, PlanState::applied);
  EXPECT_TRUE(report.alert);
  EXPECT_TRUE(action.undone.empty());
  EXPECT_EQ(stored(plan.id).state, PlanState::applied);
  // The owner takes the offer.
  ASSERT_TRUE(engine().undo(plan.id, action, Actor::owner()).has_value());
  EXPECT_EQ(stored(plan.id).state, PlanState::rolled_back);
}

TEST_F(PolicyTest, WorseIrreversibleResultIsKeptWithAnAlert) {
  const Plan plan = automatic();
  ScriptedAction action;
  action.effect = Effect::worse;
  action.reversible = false;
  const RunReport report = engine().execute(plan.id, action, Actor::scheduler(), true).value();
  EXPECT_EQ(report.state, PlanState::applied);
  EXPECT_TRUE(report.alert);
  EXPECT_TRUE(action.undone.empty());
}

TEST_F(PolicyTest, EqualResultVerified) {
  const Plan plan = automatic();
  ScriptedAction action;
  action.effect = Effect::unchanged;
  EXPECT_EQ(engine().execute(plan.id, action, Actor::scheduler(), true)->state,
            PlanState::verified);
  const LogEntry verify = log(plan.id).at(log(plan.id).size() - 2);
  EXPECT_EQ(verify.event, "verify");
  EXPECT_EQ(verify.outcome, "unchanged");
  EXPECT_EQ(verify.metrics, "volume.free before=1 after=2");
}

TEST_F(PolicyTest, FailedApplyMovesThePlanToFailed) {
  const Plan plan = automatic();
  ScriptedAction action;
  action.apply_fails = true;
  EXPECT_EQ(engine().execute(plan.id, action, Actor::scheduler(), true).error().code,
            Errc::io_error);
  EXPECT_EQ(stored(plan.id).state, PlanState::failed);
  EXPECT_EQ(log(plan.id).back().reason, "disk gone");
}

}  // namespace
}  // namespace myclean::actions
