// The aged Recycle Bin items in the service (cleaning spec, "Aged Recycle Bin Items"): the
// executor sizes them from the owner's $I records and proposes one user-scope plan, which
// user_tasks.next hands to the tray as a recycle_bin task; over a real store in a temporary
// folder and fakes.

#include "core/cleanup/recycle_bin.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "actions/actions_test_support.hpp"
#include "clean/clean_fakes.hpp"
#include "cleanup/fakes.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/actions/schema.hpp"
#include "core/actions/store_plan_source.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/tray_tasks.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/plan_repository.hpp"
#include "core/store/store.hpp"
#include "core/user_tasks/task.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::cleaning {
namespace {

using namespace myclean::literals;
using namespace std::chrono_literals;
using Json = nlohmann::json;
using cleanup::testing::bin_record;

const WallTime now = std::chrono::sys_days{std::chrono::November / 2 / 2026} + 9h;

rules::CleaningRule bin_rule(std::string id, Tier tier, std::optional<int> days) {
  rules::CleaningRule rule{.id = std::move(id),
                           .category = "windows",
                           .tier = tier,
                           .scope = ExecutionContext::user,
                           .strategy = rules::Strategy::handler,
                           .handler = "Recycle Bin"};
  rule.require.min_age_days = days;
  return rule;
}

cleanup::BinFile bin_file(std::string_view name, std::uint64_t size, WallTime deleted,
                          bool content = true) {
  return {.path = std::filesystem::path(LR"(E:\$Recycle.Bin\S-1-5-21-1-1001)") /
                  std::filesystem::path(std::string("$I") + std::string(name)),
          .data = bin_record(2, size, deleted, LR"(E:\HiveLLM\Synap\target)"),
          .has_content = content};
}

class RecycleBinPlanTest : public ::testing::Test {
 protected:
  RecycleBinPlanTest() {
    const std::vector<store::MigrationSet> sets = {actions::action_migrations(),
                                                   cleaning_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
    plans_ = std::make_unique<actions::StorePlanSource>(*store_);
    safe_remove_.emplace(
        safety::SafeRemove::create(fs_, *plans_, actions::testing::owner_locations()).value());
    engine_ = std::make_unique<actions::PolicyEngine>(*store_, clock_);
    const actions::Environment environment{.store = store_.get(),
                                           .file_system = &fs_,
                                           .safe_remove = &*safe_remove_,
                                           .clock = &clock_};
    quarantine_ = std::make_unique<actions::Quarantine>(environment, *engine_);
    executor_ = std::make_unique<Executor>(
        environment, *engine_, *quarantine_,
        Probes{.tree = &tree_, .environment = &env_, .usage = &usage_, .bin = &records_},
        actions::testing::owner_locations(), ExecutorOptions{});
    tasks_ = std::make_unique<TrayTasks>(*store_, *engine_, clock_);
  }

  ScanResult scan(const std::vector<rules::CleaningRule>& rules) {
    Result<ScanResult> result = executor_->scan(RuleSet{.rules = rules}, true, {});
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(ScanResult{});
  }

  // The plans of `result` with their specs.
  std::vector<std::pair<Plan, PlanSpec>> plans(const ScanResult& result) {
    std::vector<std::pair<Plan, PlanSpec>> found;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      for (const PlanId id : result.plans) {
                        Plan plan = store::get_plan(db, id).value();
                        PlanSpec spec = spec_of(db, plan).value();
                        found.emplace_back(std::move(plan), std::move(spec));
                      }
                      return {};
                    })
                    .has_value());
    return found;
  }

  cleanup::testing::FakeBinRecords& records() { return records_; }
  TrayTasks& tasks() { return *tasks_; }
  fakes::FakeClock& clock() { return clock_; }

 private:
  actions::testing::TempFolder folder_;
  fakes::FakeClock clock_{now};
  fakes::FakeFileSystem fs_;
  clean::testing::FakeTree tree_;
  clean::testing::FakeEnvironment env_;
  clean::testing::FakeUsage usage_;
  cleanup::testing::FakeBinRecords records_;
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<actions::StorePlanSource> plans_;
  std::optional<safety::SafeRemove> safe_remove_;
  std::unique_ptr<actions::PolicyEngine> engine_;
  std::unique_ptr<actions::Quarantine> quarantine_;
  std::unique_ptr<Executor> executor_;
  std::unique_ptr<TrayTasks> tasks_;
};

// The plan is sized from the $I records deleted 30 days ago or earlier whose content is left; the
// whole bin stays an ask-tier handler plan of its own.
TEST_F(RecycleBinPlanTest, AgedItemsBecomeOneApprovedUserPlanSizedFromTheRecords) {
  records().files = {bin_file("AAA", 41'750'000'000, now - 31 * 24h),
                     bin_file("BBB", 6'500'000'000, now - 30 * 24h),
                     bin_file("CCC", 190'000'000, now - 12 * 24h),
                     bin_file("DDD", 23'500'000'000, now - 140 * 24h, false)};

  const ScanResult result = scan({bin_rule("windows/recycle-bin", Tier::ask, std::nullopt),
                                  bin_rule("windows/recycle-bin-aged", Tier::automatic, 30)});

  EXPECT_TRUE(result.problems.empty());
  const auto found = plans(result);
  ASSERT_EQ(found.size(), 2U);
  const auto aged = std::ranges::find(found, std::string("RecycleBinCleanup"),
                                      [](const auto& entry) { return entry.first.action; });
  ASSERT_NE(aged, found.end());
  const auto& [plan, spec] = *aged;
  EXPECT_EQ(plan.state, PlanState::approved);
  ASSERT_EQ(plan.items.size(), 1U);
  EXPECT_EQ(plan.items[0].path, cleanup::recycle_bin_item(30));
  EXPECT_EQ(plan.items[0].size, Bytes{41'750'000'000 + 6'500'000'000});
  EXPECT_EQ(spec.kind, PlanKind::recycle_bin);
  EXPECT_EQ(spec.tier, Tier::automatic);
  EXPECT_EQ(spec.scope, ExecutionContext::user);
  EXPECT_EQ(spec.params, (Json{{"deleted_days", 30}}));
  EXPECT_EQ(spec.undo, "not reversible: the items are deleted permanently");

  const auto whole = std::ranges::find(found, std::string("HandlerCleanup"),
                                       [](const auto& entry) { return entry.first.action; });
  ASSERT_NE(whole, found.end());
  EXPECT_EQ(whole->first.state, PlanState::awaiting_approval);
  EXPECT_EQ(whole->second.params.at("handlers"), Json::array({"Recycle Bin"}));
}

TEST_F(RecycleBinPlanTest, NothingDeletedLongEnoughAgoProposesNothing) {
  records().files = {bin_file("CCC", 190'000'000, now - 12 * 24h),
                     bin_file("DDD", 23'500'000'000, now - 140 * 24h, false)};

  const ScanResult result = scan({bin_rule("windows/recycle-bin-aged", Tier::automatic, 30)});

  EXPECT_TRUE(result.plans.empty());
  EXPECT_EQ(result.candidates, 0);
  EXPECT_TRUE(result.problems.empty());
}

TEST_F(RecycleBinPlanTest, AnUnreadableBinOrTooYoungAnAgeIsAProblemAndNoPlan) {
  records().failure = Error{.code = Errc::access_denied, .message = "FindFirstFileEx failed"};
  const ScanResult unreadable = scan({bin_rule("windows/recycle-bin-aged", Tier::automatic, 30)});
  EXPECT_TRUE(unreadable.plans.empty());
  ASSERT_EQ(unreadable.problems.size(), 1U);
  EXPECT_EQ(unreadable.problems[0], "Recycle Bin: FindFirstFileEx failed");

  records().failure.reset();
  records().files = {bin_file("AAA", 1'000, now - 31 * 24h)};
  const int reads = records().reads;
  const ScanResult young = scan({bin_rule("owner/bin-3-days", Tier::automatic, 3)});
  EXPECT_TRUE(young.plans.empty());
  ASSERT_EQ(young.problems.size(), 1U);
  EXPECT_NE(young.problems[0].find("at least 7"), std::string::npos) << young.problems[0];
  EXPECT_EQ(records().reads, reads) << "not even read";
}

// user_tasks.next hands the plan to the tray as a recycle_bin task, which reads back exactly; the
// tray's report makes it verified.
TEST_F(RecycleBinPlanTest, TheTrayGetsARecycleBinTaskAndItsReportIsRecorded) {
  records().files = {bin_file("AAA", 5'000, now - 31 * 24h)};
  const ScanResult result = scan({bin_rule("windows/recycle-bin-aged", Tier::automatic, 30)});
  ASSERT_EQ(result.plans.size(), 1U);

  const Result<Json> next = tasks().next();
  ASSERT_TRUE(next.has_value()) << next.error().message;
  const Json& task = next->at("task");
  EXPECT_EQ(task.at("kind"), "recycle_bin");
  EXPECT_EQ(task.at("action"), (Json{{"deleted_days", 30}}));
  const Result<user_tasks::UserTask> parsed = user_tasks::parse_task(task);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->kind, user_tasks::TaskKind::recycle_bin);
  EXPECT_EQ(parsed->deleted_days, 30);
  EXPECT_EQ(parsed->tier, Tier::automatic);
  ASSERT_EQ(parsed->items.size(), 1U);
  EXPECT_EQ(parsed->items[0].path, cleanup::recycle_bin_item(30));

  cleanup::testing::FakeRecycleBin bin;
  bin.contents = {{.id = R"(E:\$RECYCLE.BIN\S-1-5-21-1-1001\$RAAA)",
                   .name = R"(E:\HiveLLM\Synap\target)",
                   .deleted = now - 31 * 24h,
                   .size = Bytes{5'000}}};
  const user_tasks::TaskReport report = user_tasks::run(
      *parsed, user_tasks::Toolbox{.recycle_bin = &bin, .clock = &clock()}, std::stop_token{});
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_TRUE(bin.contents.empty());

  const Result<PlanState> state =
      tasks().report(result.plans[0], user_tasks::report_record(report));
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, PlanState::verified);
}

}  // namespace
}  // namespace myclean::cleaning
