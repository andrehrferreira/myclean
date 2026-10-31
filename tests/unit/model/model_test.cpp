#include <chrono>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"

namespace myclean {
namespace {

using namespace myclean::literals;

TEST(Bytes, LiteralsUseBinaryMultiples) {
  EXPECT_EQ((1_KiB).count(), 1024U);
  EXPECT_EQ((3_MiB).count(), 3U * 1024U * 1024U);
  EXPECT_EQ((2_GiB).count(), 2ULL * 1024U * 1024U * 1024U);
}

TEST(Bytes, AddsAndCompares) {
  Bytes total = 512_B;
  total += 512_B;
  EXPECT_EQ(total, 1_KiB);
  EXPECT_LT(1_KiB, 1_KiB + 1_B);
  EXPECT_EQ(Bytes{}.count(), 0U);
}

TEST(Id, ZeroMeansNotStoredAndIdsOrderByValue) {
  EXPECT_EQ(PlanId{}.value(), 0);
  EXPECT_LT(PlanId{1}, PlanId{2});
  EXPECT_EQ(PlanItemId{7}, PlanItemId{7});
}

TEST(Result, FailCarriesCodeAndMessage) {
  const Result<int> result = fail(Errc::not_found, "no such plan");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::not_found);
  EXPECT_EQ(result.error().message, "no such plan");
  EXPECT_EQ(to_string(Errc::not_reversible), "not_reversible");
  EXPECT_EQ(to_string(Errc::io_error), "io_error");
}

TEST(Result, VoidSuccessHasValue) {
  const Result<void> result{};
  EXPECT_TRUE(result.has_value());
}

TEST(Actor, TextFormRoundTrips) {
  for (const char* text : {"scheduler", "governor", "owner", "mcp:claude-code"}) {
    const Result<Actor> actor = Actor::parse(text);
    ASSERT_TRUE(actor.has_value()) << text;
    EXPECT_EQ(actor->to_string(), text);
  }
}

TEST(Actor, McpActorKeepsClientName) {
  const Result<Actor> actor = Actor::mcp("claude-code");
  ASSERT_TRUE(actor.has_value());
  EXPECT_EQ(actor->kind(), Actor::Kind::mcp);
  EXPECT_EQ(actor->client(), "claude-code");
  EXPECT_NE(*actor, Actor::owner());
}

TEST(Actor, RejectsMalformedText) {
  for (const char* text : {"", "Owner", "mcp:", "mcp:bad\nname", "system"}) {
    const Result<Actor> actor = Actor::parse(text);
    ASSERT_FALSE(actor.has_value()) << text;
    EXPECT_EQ(actor.error().code, Errc::invalid_argument);
  }
}

TEST(PlanState, TextFormsMatchTheArchitecture) {
  const std::map<PlanState, std::string> expected = {
      {PlanState::proposed, "proposed"},
      {PlanState::automatic, "auto"},
      {PlanState::awaiting_approval, "awaiting_approval"},
      {PlanState::approved, "approved"},
      {PlanState::applying, "applying"},
      {PlanState::applied, "applied"},
      {PlanState::verified, "verified"},
      {PlanState::rolled_back, "rolled_back"},
      {PlanState::failed, "failed"},
      {PlanState::rejected, "rejected"},
  };
  for (const auto& [state, text] : expected) {
    EXPECT_EQ(to_string(state), text);
    EXPECT_EQ(parse_plan_state(text), state);
  }
  EXPECT_FALSE(parse_plan_state("cancelled").has_value());
}

TEST(EnumText, EveryEnumRoundTrips) {
  for (const ItemApproval value :
       {ItemApproval::pending, ItemApproval::approved, ItemApproval::not_approved}) {
    EXPECT_EQ(parse_item_approval(to_string(value)), value);
  }
  for (const Severity value : {Severity::info, Severity::warning, Severity::critical}) {
    EXPECT_EQ(parse_severity(to_string(value)), value);
  }
  for (const Risk value : {Risk::low, Risk::medium, Risk::high}) {
    EXPECT_EQ(parse_risk(to_string(value)), value);
  }
  for (const Effect value : {Effect::better, Effect::unchanged, Effect::worse}) {
    EXPECT_EQ(parse_effect(to_string(value)), value);
  }
  EXPECT_EQ(to_string(Tier::automatic), "auto");
  EXPECT_EQ(parse_tier("ask"), Tier::ask);
  EXPECT_EQ(to_string(ExecutionContext::service), "svc");
  EXPECT_EQ(parse_execution_context("user"), ExecutionContext::user);
  EXPECT_FALSE(parse_tier("Auto").has_value());
}

TEST(Plan, NewPlanStartsProposedWithPendingItems) {
  const Plan plan{.id = PlanId{42},
                  .action = "DeleteItems",
                  .proposer = Actor::scheduler(),
                  .items = {PlanItem{.id = PlanItemId{1}, .path = L"C:\\Temp\\a.tmp"}}};
  EXPECT_EQ(plan.state, PlanState::proposed);
  EXPECT_FALSE(plan.finding.has_value());
  EXPECT_EQ(plan.items.front().approval, ItemApproval::pending);
}

TEST(Preview, TotalSizeSumsItems) {
  const Preview preview{.items = {PlanItem{.id = PlanItemId{1}, .size = 1_GiB},
                                  PlanItem{.id = PlanItemId{2}, .size = 200_MiB}}};
  EXPECT_EQ(preview.total_size(), 1_GiB + 200_MiB);
}

TEST(Undo, ReversibleUnlessAReasonIsGiven) {
  EXPECT_TRUE(Undo{}.reversible());
  EXPECT_FALSE(Undo{.not_reversible = "regenerates"}.reversible());
}

TEST(SeriesKey, OrdersByMetricThenEntity) {
  EXPECT_LT((SeriesKey{.metric = "cpu.core", .entity = "3"}),
            (SeriesKey{.metric = "cpu.core", .entity = "4"}));
  EXPECT_LT((SeriesKey{.metric = "cpu.core", .entity = "9"}),
            (SeriesKey{.metric = "mem.available", .entity = "0"}));
  const Series series{
      .key = {.metric = "cpu.core", .entity = "3"},
      .samples = {Sample{.time = WallTime{std::chrono::seconds{1}}, .value = 10.0}}};
  EXPECT_EQ(series.samples.size(), 1U);
}

TEST(Finding, DefaultsToInfoSeverity) {
  const Finding finding{.kind = "volume.fill_up", .subject = "C:"};
  EXPECT_EQ(finding.severity, Severity::info);
  EXPECT_EQ(finding.id, FindingId{});
}

}  // namespace
}  // namespace myclean
