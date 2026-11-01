#include "core/tray_model/cleaning.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/format.hpp"
#include "core/tray_model/replies.hpp"
#include "inbox_fixtures.hpp"

namespace myclean::tray_model {
namespace {

using Json = nlohmann::json;
using testing::shader_bytes;

TEST(CleaningRepliesTest, CleaningStatusWhileScanningAndIdle) {
  const Result<CleaningStatus> scanning =
      parse_cleaning_status(testing::cleaning_status_json(true));
  ASSERT_TRUE(scanning.has_value()) << scanning.error().message;
  EXPECT_TRUE(scanning->scanning);
  EXPECT_DOUBLE_EQ(scanning->progress, 0.4);
  EXPECT_EQ(scanning->last_completed, from_unix_seconds(990'000));
  EXPECT_EQ(scanning->candidates, 42);
  EXPECT_EQ(scanning->bytes, Bytes{5'000'000'000ULL});
  EXPECT_EQ(scanning->next_auto_run, from_unix_seconds(1'050'000));
  EXPECT_EQ(scanning->purging, (std::vector<std::int64_t>{3}));
  EXPECT_TRUE(scanning->unavailable.empty());

  Json never = testing::cleaning_status_json(false);
  never.erase("purging");  // a service older than quarantine.purge
  never["last_completed"] = nullptr;
  never["unavailable"] = "the service runs without its store";
  const Result<CleaningStatus> idle = parse_cleaning_status(never);
  ASSERT_TRUE(idle.has_value());
  EXPECT_FALSE(idle->scanning);
  EXPECT_FALSE(idle->last_completed);
  EXPECT_EQ(idle->unavailable, "the service runs without its store");
  EXPECT_TRUE(idle->purging.empty());

  never["state"] = "busy";
  EXPECT_FALSE(parse_cleaning_status(never).has_value());
}

TEST(CleaningRepliesTest, ApplyingShowsTheProgressOfThePlanAndIsAbsentFromAnOlderService) {
  Json status = testing::cleaning_status_json(false);
  const Result<CleaningStatus> older = parse_cleaning_status(status);  // no "applying" member
  ASSERT_TRUE(older.has_value()) << older.error().message;
  EXPECT_FALSE(older->applying);
  status["applying"] = nullptr;
  EXPECT_FALSE(parse_cleaning_status(status)->applying);

  status["applying"] = {{"kind", "plan"},
                        {"plan", 7},
                        {"title", "Quarantine dev-caches"},
                        {"started_at", 1'000'000},
                        {"items_done", 3},
                        {"items_total", 12},
                        {"bytes_done", 1'500'000'000},
                        {"bytes_total", 6'000'000'000},
                        {"files_done", 812},
                        {"current", R"(C:\zig\cache\o\1f2e)"}};
  const Result<CleaningStatus> running = parse_cleaning_status(status);
  ASSERT_TRUE(running.has_value()) << running.error().message;
  ASSERT_TRUE(running->applying);
  const ApplyingRow& applying = *running->applying;
  EXPECT_EQ(applying.plan, 7);
  EXPECT_EQ(applying.files_done, 812);
  EXPECT_DOUBLE_EQ(applied_share(applying), 0.25);
  EXPECT_EQ(applying_text(applying), "Applying Quarantine dev-caches: 3 of 12 items, " +
                                         format_bytes(1.5e9) + " of " + format_bytes(6e9) +
                                         R"( - C:\zig\cache\o\1f2e)");
  EXPECT_EQ(applying_elapsed(applying, from_unix_seconds(1'000'045)), "45 s");
  EXPECT_EQ(applying_elapsed(applying, from_unix_seconds(1'000'725)), "12 min 5 s");
  EXPECT_EQ(applying_elapsed(applying, from_unix_seconds(1'003'780)), "1 h 3 min");

  status["applying"] = {{"kind", "purge"},   {"plan", nullptr},  {"title", "Purge the quarantine"},
                        {"started_at", 1},   {"items_done", 0},  {"items_total", 2},
                        {"bytes_done", 0},   {"bytes_total", 0}, {"files_done", 0},
                        {"current", nullptr}};
  const Result<CleaningStatus> purging = parse_cleaning_status(status);
  ASSERT_TRUE(purging.has_value()) << purging.error().message;
  ASSERT_TRUE(purging->applying);
  const ApplyingRow& scheduled = *purging->applying;
  EXPECT_FALSE(scheduled.plan.has_value());
  EXPECT_TRUE(scheduled.current.empty());
  EXPECT_DOUBLE_EQ(applied_share(scheduled), 0.0);

  status["applying"].erase("title");
  EXPECT_FALSE(parse_cleaning_status(status).has_value());
}

// Approvals spec, "Pending plan shown".
TEST(CleaningRepliesTest, PlansListShowsItemsBytesTierAndProposer) {
  const Result<std::vector<PlanRow>> plans = parse_plans_list(testing::plans_json());
  ASSERT_TRUE(plans.has_value()) << plans.error().message;
  ASSERT_EQ(plans->size(), 4U);
  const PlanRow& shader = plans->front();
  EXPECT_EQ(shader.id, 7);
  EXPECT_EQ(shader.title, "Shader caches");
  EXPECT_EQ(shader.items, 12);
  EXPECT_EQ(shader.bytes, Bytes{shader_bytes});
  EXPECT_EQ(shader.tier, "ask");
  EXPECT_EQ(shader.proposer, "scheduler");
  EXPECT_EQ(shader.state, "awaiting_approval");
  EXPECT_EQ(shader.created_at, from_unix_seconds(1'000'000));
  EXPECT_EQ((*plans)[1].scope, "user");
  EXPECT_EQ(pending_count(*plans), 2);

  Json broken = testing::plans_json();
  broken["plans"][0].erase("tier");
  EXPECT_EQ(parse_plans_list(broken).error().code, Errc::invalid_data);
}

TEST(CleaningRepliesTest, PlansGetHasItemsCommandsUndoAndWarnings) {
  Json answer = testing::plan_get_json(7);
  answer["items"][1]["approved"] = true;
  answer["items"][2]["approved"] = false;
  const Result<PlanDetail> detail = parse_plans_get(answer);
  ASSERT_TRUE(detail.has_value()) << detail.error().message;
  EXPECT_EQ(detail->plan.id, 7);
  EXPECT_EQ(detail->plan.items, 12);
  ASSERT_EQ(detail->items.size(), 12U);
  EXPECT_EQ(detail->items[0].id, 100);
  EXPECT_FALSE(detail->items[0].rule);
  EXPECT_EQ(detail->items[1].rule, "gpu.nvidia_dx");
  EXPECT_EQ(detail->items[1].reason, "shader cache, regenerates");
  EXPECT_FALSE(detail->items[0].approved);
  EXPECT_EQ(detail->items[1].approved, true);
  EXPECT_EQ(detail->items[2].approved, false);
  EXPECT_EQ(detail->commands.size(), 1U);
  EXPECT_EQ(detail->undo, "restore from quarantine within 7 days");
  EXPECT_EQ(detail->warnings.size(), 1U);
  EXPECT_FALSE(detail->freed);
  EXPECT_TRUE(detail->outcome.empty());

  answer["freed"] = 4096;
  answer["detail"] = "rolled back: verification worse";
  const Result<PlanDetail> applied = parse_plans_get(answer);
  ASSERT_TRUE(applied.has_value());
  EXPECT_EQ(applied->freed, Bytes{4096});
  EXPECT_EQ(applied->outcome, "rolled back: verification worse");
}

// Approvals spec, "Approve some": only the ticked items are approved.
TEST(CleaningRepliesTest, ApproveSomeNamesTheTickedItemsOrTheFewUnticked) {
  const PlanDetail small = *parse_plans_get(testing::plan_get_json(7));
  EXPECT_EQ(plans_approve_params(7, select_all()),
            (Json{{"id", 7}, {"selection", {{"all", true}}}}));
  const Result<Selection> some = select_items(small, {100, 102});
  ASSERT_TRUE(some.has_value());
  EXPECT_EQ(plans_approve_params(7, *some),
            (Json{{"id", 7}, {"selection", {{"all", false}, {"items", {100, 102}}}}}));
  EXPECT_FALSE(select_items(small, {}).has_value()) << "approving none is Reject";

  // 100 items: 98 ticked are sent as all but the 2 unticked.
  const PlanDetail large = *parse_plans_get(testing::plan_get_json(9, 100));
  std::set<std::int64_t> ticked;
  for (std::int64_t id = 100; id < 200; ++id) {
    ticked.insert(id);
  }
  ticked.erase(150);
  ticked.erase(151);
  const Result<Selection> most = select_items(large, ticked);
  ASSERT_TRUE(most.has_value());
  EXPECT_TRUE(most->all);
  EXPECT_EQ(most->except, (std::vector<std::int64_t>{150, 151}));
  for (const Selection& selection : {select_all(), *some, *most}) {
    const Json record = plans_approve_params(9, selection).at("selection");
    EXPECT_EQ(ipc::check_selection(record), "");
    EXPECT_EQ(ipc::check_record_bounds(record), "");
  }

  // Half of 140: neither list fits a record.
  const PlanDetail huge = *parse_plans_get(testing::plan_get_json(9, 140));
  std::set<std::int64_t> half;
  for (std::int64_t id = 100; id < 170; ++id) {
    half.insert(id);
  }
  EXPECT_FALSE(select_items(huge, half).has_value());

  // A plan with items beyond the listed ones is never approved as "all but".
  PlanDetail partial = large;
  partial.plan.items = 3000;
  EXPECT_FALSE(select_items(partial, ticked).has_value());
}

TEST(CleaningRepliesTest, RequestParameters) {
  EXPECT_EQ(plan_id_params(7), (Json{{"id", 7}}));
  EXPECT_EQ(propose_cleanup_params({701, 702}), (Json{{"candidates", {{"ids", {701, 702}}}}}));
  EXPECT_EQ(ipc::check_candidates(propose_cleanup_params({701}).at("candidates")), "");
  EXPECT_EQ(quarantine_restore_params(3, std::nullopt), (Json{{"plan", 3}}));
  EXPECT_EQ(quarantine_restore_params(3, 31), (Json{{"plan", 3}, {"item", 31}}));
  EXPECT_EQ(space_params(SpaceList::top, "C:", false), (Json{{"path", "C"}}));
  EXPECT_EQ(space_params(SpaceList::top, "D:", true), (Json{{"path", "D"}, {"refresh", true}}));
  EXPECT_EQ(space_params(SpaceList::duplicates, "C:", false), Json::object());
  EXPECT_EQ(space_method(SpaceList::used_installers), "space.used_installers");
}

TEST(CleaningRepliesTest, QuarantineRunsWithPurgeDates) {
  const Result<std::vector<QuarantineRun>> runs = parse_quarantine_list(testing::quarantine_json());
  ASSERT_TRUE(runs.has_value()) << runs.error().message;
  ASSERT_EQ(runs->size(), 1U);
  const QuarantineRun& run = runs->front();
  EXPECT_EQ(run.plan, 3);
  EXPECT_EQ(run.purge_after, from_unix_seconds(900'000 + (7 * 86'400)));
  ASSERT_EQ(run.items.size(), 2U);
  EXPECT_EQ(run.items[0].item, 31);
  EXPECT_EQ(run.items[0].original, R"(F:\app\node_modules)");
  EXPECT_EQ(run.items[1].bytes, Bytes{1000});
}

TEST(CleaningRepliesTest, SpaceListsWithCandidateIds) {
  SpaceReply top{.list = SpaceList::top, .volume = "C:"};
  ASSERT_TRUE(parse_space(testing::space_top_json(), top).has_value());
  EXPECT_EQ(top.state, "ready");
  EXPECT_EQ(top.scanned_at, from_unix_seconds(999'000));
  ASSERT_EQ(top.top_files.size(), 1U);
  EXPECT_EQ(top.top_files[0].id, 501);
  EXPECT_EQ(top.top_folders[0].files, 300'000U);

  SpaceReply downloads{.list = SpaceList::old_downloads};
  ASSERT_TRUE(parse_space(testing::old_downloads_json(), downloads).has_value());
  ASSERT_EQ(downloads.downloads.size(), 2U);
  EXPECT_EQ(downloads.downloads[0].origin, "https://example.com/setup.zip");
  EXPECT_EQ(downloads.downloads[0].age_days, 120);
  EXPECT_FALSE(downloads.downloads[1].origin) << "no Zone.Identifier: origin unknown";

  SpaceReply duplicates{.list = SpaceList::duplicates};
  ASSERT_TRUE(parse_space(testing::duplicates_json(), duplicates).has_value());
  ASSERT_EQ(duplicates.duplicates.size(), 1U);
  const DuplicateFileRow* keep = keep_hint(duplicates.duplicates[0]);
  ASSERT_NE(keep, nullptr);
  EXPECT_EQ(keep->id, 702) << "the oldest copy";

  SpaceReply installers{.list = SpaceList::used_installers};
  ASSERT_TRUE(parse_space(testing::used_installers_json(), installers).has_value());
  ASSERT_EQ(installers.installers.size(), 1U);
  EXPECT_EQ(installers.installers[0].installed_as, "Tool 1.2");

  // The first analysis still runs: no result yet.
  SpaceReply first{.list = SpaceList::duplicates};
  ASSERT_TRUE(parse_space(testing::space_state("scanning"), first).has_value());
  EXPECT_EQ(first.state, "scanning");
  EXPECT_TRUE(first.duplicates.empty());

  SpaceReply broken{.list = SpaceList::top};
  Json bad = testing::space_top_json();
  bad["files"][0].erase("id");
  EXPECT_FALSE(parse_space(bad, broken).has_value());
}

TEST(CleaningRepliesTest, InboxViews) {
  const std::vector<PlanRow> plans = *parse_plans_list(testing::plans_json());
  EXPECT_EQ(pending_bytes_by_tier(plans),
            (ByteTotals{{"ask", Bytes{shader_bytes}}, {"auto", Bytes{shader_bytes}}}));
  EXPECT_EQ(pending_bytes_by_kind(plans),
            (ByteTotals{{"cleanup", Bytes{shader_bytes}}, {"native", Bytes{shader_bytes}}}));
  EXPECT_FALSE(undoable(plans[0])) << "waiting";
  EXPECT_TRUE(undoable(plans[2])) << "a verified relocation";
  EXPECT_FALSE(undoable(plans[3])) << "failed";
  PlanRow native = plans[1];
  native.state = "verified";
  EXPECT_FALSE(undoable(native)) << "a prune regenerates";
}

TEST(CleanupViewTest, KeepsDetailsOfListedPlansAndTheNewestOfEachList) {
  CleanupView view;
  CleanupPoll first;
  first.plans = *parse_plans_list(testing::plans_json());
  first.details.push_back(*parse_plans_get(testing::plan_get_json(7)));
  first.details.push_back(*parse_plans_get(testing::plan_get_json(5, 1, "verified")));
  first.action = "Approve failed: invalid_state";
  SpaceReply duplicates{.list = SpaceList::duplicates};
  ASSERT_TRUE(parse_space(testing::duplicates_json(), duplicates).has_value());
  first.space.push_back(std::move(duplicates));
  first.task_activity = "Running plan 8: Run `pnpm store prune`";
  view.apply(std::move(first));
  EXPECT_EQ(view.pending(), 2);
  ASSERT_NE(view.detail(7), nullptr);
  EXPECT_EQ(view.detail(7)->items.size(), 12U);
  EXPECT_EQ(view.action_error(), "Approve failed: invalid_state");
  ASSERT_TRUE(view.space(SpaceList::duplicates).has_value());
  EXPECT_FALSE(view.space(SpaceList::top).has_value());
  EXPECT_FALSE(view.task_activity().empty());

  // Plan 5 left the list: its details go. An empty action clears the error.
  CleanupPoll second;
  Json listed = testing::plans_json();
  listed["plans"].erase(2);
  second.plans = *parse_plans_list(listed);
  second.action = "";
  second.task_activity = "";
  second.task_note = "Plan 8 (Run `pnpm store prune`): done";
  view.apply(std::move(second));
  EXPECT_NE(view.detail(7), nullptr);
  EXPECT_EQ(view.detail(5), nullptr);
  EXPECT_TRUE(view.action_error().empty());
  EXPECT_TRUE(view.task_activity().empty());
  EXPECT_EQ(view.task_note(), "Plan 8 (Run `pnpm store prune`): done");
  EXPECT_TRUE(view.space(SpaceList::duplicates).has_value()) << "kept until asked again";
}

}  // namespace
}  // namespace myclean::tray_model
