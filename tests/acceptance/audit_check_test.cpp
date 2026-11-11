#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "acceptance_fixture.hpp"
#include "core/actions/audit_log.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"

// The check every prompt ends with: no action in action_log without the owner's approval in the
// tray. These tests show that it catches such an action and names it, and that no MCP tool can
// approve, execute or undo a plan.
namespace myclean::acceptance {
namespace {

// Appends `entry` to action_log, as a component that acted would.
void append(store::Store& store, const actions::LogEntry& entry) {
  ASSERT_TRUE(store
                  .write([&](store::Database& db) -> Result<void> {
                    return actions::append_log(db, entry).transform([](std::int64_t) {});
                  })
                  .has_value());
}

// Spec "Suite fails on an unapproved action".
TEST_F(AcceptanceTest, TheAuditCheckFailsAndNamesAnActionWithoutATrayApproval) {
  static_cast<void>(tool("list_startup"));
  const Json proposed = tool("propose_disable_startup", Json{{"item_ids", {1}}});
  ASSERT_EQ(proposed.at("state"), "awaiting_approval");
  const std::int64_t plan = proposed.at("plan").get<std::int64_t>();
  EXPECT_TRUE(unapproved_actions(store()).empty());  // a proposal is no action

  // A run in which the plan was applied although the owner never approved it in the tray.
  append(store(), actions::LogEntry{.time = now,
                                    .plan = PlanId{plan},
                                    .actor = "scheduler",
                                    .event = "apply",
                                    .outcome = "disabled",
                                    .detail = "run:user:Discord"});
  const std::vector<std::string> found = unapproved_actions(store());
  ASSERT_EQ(found.size(), 1U);
  const std::string named = "plan " + std::to_string(plan) + ": apply disabled by scheduler";
  EXPECT_TRUE(found[0].starts_with(named)) << found[0];
  EXPECT_NONFATAL_FAILURE(expect_every_action_approved(store()),
                          "action without a tray approval: " + named);
}

// An approval that is not the owner's (an MCP client, the scheduler) approves nothing.
TEST_F(AcceptanceTest, OnlyTheOwnersApprovalInTheTrayCountsForAnAction) {
  static_cast<void>(tool("list_startup"));
  const std::int64_t plan =
      tool("propose_disable_startup", Json{{"item_ids", {2}}}).at("plan").get<std::int64_t>();
  append(store(), actions::LogEntry{.time = now,
                                    .plan = PlanId{plan},
                                    .actor = "mcp:claude-code",
                                    .event = "transition",
                                    .from = PlanState::awaiting_approval,
                                    .to = PlanState::approved,
                                    .outcome = "accepted"});
  append(store(), actions::LogEntry{.time = now,
                                    .plan = PlanId{plan},
                                    .actor = "mcp:claude-code",
                                    .event = "transition",
                                    .from = PlanState::approved,
                                    .to = PlanState::applying,
                                    .outcome = "accepted"});
  const std::vector<std::string> found = unapproved_actions(store());
  ASSERT_EQ(found.size(), 2U);
  EXPECT_TRUE(found[0].starts_with("plan " + std::to_string(plan) +
                                   ": transition approved by mcp:claude-code"))
      << found[0];
  EXPECT_TRUE(found[1].starts_with("plan " + std::to_string(plan) +
                                   ": transition applying by mcp:claude-code"))
      << found[1];

  // And a client other than the tray cannot make that approval.
  EXPECT_FALSE(other_call("plans.approve", Json{{"id", plan}, {"selection", {{"all", true}}}}));
}

// A governor step or any action without a plan never has a tray approval.
TEST_F(AcceptanceTest, AnActionWithoutAPlanIsNamedToo) {
  append(store(), actions::LogEntry{.time = now,
                                    .plan = PlanId{0},
                                    .actor = "governor",
                                    .event = "GovernorStep",
                                    .outcome = "suspend",
                                    .detail = "editor.exe"});
  const std::vector<std::string> found = unapproved_actions(store());
  ASSERT_EQ(found.size(), 1U);
  EXPECT_TRUE(found[0].starts_with("plan 0: GovernorStep suspend by governor")) << found[0];
}

// UI-6: read tools and proposal tools only; none approves, executes or undoes a plan.
TEST_F(AcceptanceTest, NoMcpToolCanApproveExecuteOrUndoAPlan) {
  const Json list = tools();
  ASSERT_FALSE(list.empty());
  for (const Json& entry : list) {
    const std::string name = entry.at("name").get<std::string>();
    const bool read_only = entry.at("annotations").at("readOnlyHint").get<bool>();
    EXPECT_TRUE(read_only || name.starts_with("propose_")) << name;
    for (const char* verb : {"approve", "execute", "apply", "undo", "kill", "close", "delete"}) {
      EXPECT_EQ(name.find(verb), std::string::npos) << name;
    }
  }
}

}  // namespace
}  // namespace myclean::acceptance
