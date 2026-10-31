#include "core/user_tasks/task.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "../cleanup/fakes.hpp"
#include "../placement/placement_fakes.hpp"
#include "../tray_model/inbox_fixtures.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::user_tasks {
namespace {

using namespace literals;
using Json = nlohmann::json;

Json item(std::int64_t id, std::string_view path, Json approved) {
  return Json{{"id", id},           {"path", path},
              {"bytes", 1000},      {"approved", std::move(approved)},
              {"volume_serial", 0}, {"file_id", ""}};
}

Json handler_task(const std::vector<std::string>& handlers, const std::vector<Json>& items) {
  return Json{{"plan", 4},         {"title", "Windows disk cleanup"},
              {"kind", "handler"}, {"operation", "apply"},
              {"tier", "ask"},     {"action", {{"volume", "C:\\"}, {"handlers", handlers}}},
              {"items", items}};
}

Json relocate_task(std::string_view operation) {
  return Json{
      {"plan", 5},
      {"title", R"(Relocate pip to D:\DevCaches\pip)"},
      {"kind", "relocate"},
      {"operation", operation},
      {"tier", "ask"},
      {"action", {{"cache", "pip"}, {"target", R"(D:\DevCaches\pip)"}, {"content", "regenerate"}}},
      {"items", Json::array({item(51, "relocate:pip", true)})}};
}

class UserTaskTest : public ::testing::Test {
 protected:
  UserTaskTest() {
    drives_.list() = placement::testing::owner_volumes();
    for (const placement::TargetVolume& volume : drives_.list()) {
      fs_.add_directory(volume.root);
    }
    fs_.add_directory(LR"(C:\Users\owner)");
  }

  TaskReport run_json(const Json& task, const std::stop_token& stop = {}) {
    const Result<UserTask> parsed = parse_task(task);
    EXPECT_TRUE(parsed.has_value()) << parsed.error().message;
    return run(*parsed, tools_, stop);
  }

  cleanup::testing::FakeRunner& runner() { return runner_; }
  cleanup::testing::FakeHost& host() { return host_; }
  fakes::FakeFileSystem& fs() { return fs_; }
  placement::testing::FakeUserEnvironment& env() { return env_; }
  placement::testing::FakeDriveFacts& drives() { return drives_; }

 private:
  cleanup::testing::FakeRunner runner_;
  cleanup::testing::FakeHost host_;
  fakes::FakeFileSystem fs_;
  placement::testing::FakeUserEnvironment env_;
  placement::testing::FakeTextFiles files_;
  placement::testing::FakeTreeCopier copier_{fs_};
  placement::testing::FakeDriveFacts drives_;
  Toolbox tools_{.runner = &runner_,
                 .handlers = &host_,
                 .file_system = &fs_,
                 .placement = {.environment = &env_,
                               .files = &files_,
                               .copier = &copier_,
                               .file_system = &fs_,
                               .drives = &drives_}};
};

TEST(ParseTaskTest, ReadsANativeTaskAndDropsOnlyRejectedItems) {
  Json answer = tray_model::testing::native_task_json();
  answer["items"].push_back(item(82, "native:other", false));
  answer["items"].push_back(item(83, "native:pending", nullptr));
  const Result<UserTask> task = parse_task(answer);
  ASSERT_TRUE(task.has_value()) << task.error().message;
  EXPECT_EQ(task->plan, 8);
  EXPECT_EQ(task->title, "Run `pnpm store prune`");
  EXPECT_EQ(task->kind, TaskKind::native);
  EXPECT_EQ(task->operation, Operation::apply);
  EXPECT_EQ(task->tier, Tier::automatic);
  EXPECT_EQ(task->command, "pnpm store prune");
  ASSERT_EQ(task->items.size(), 2U);
  EXPECT_EQ(task->items[0].id, 81);
  EXPECT_EQ(task->items[1].id, 83);
}

TEST(ParseTaskTest, RefusesKindsTheTrayDoesNotRunAndUnreadableTasks) {
  Json recycle = tray_model::testing::native_task_json();
  recycle["kind"] = "cleanup";
  recycle["action"] = {{"recycle", true}};
  const Result<UserTask> refused = parse_task(recycle);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, Errc::refused);
  EXPECT_EQ(task_plan(recycle).value(), 8);

  Json broken = tray_model::testing::native_task_json();
  broken["action"].erase("command");
  EXPECT_EQ(parse_task(broken).error().code, Errc::invalid_data);
  EXPECT_FALSE(task_plan(Json{{"title", "no plan"}}).has_value());
  EXPECT_FALSE(task_plan(Json(nullptr)).has_value());
}

TEST(ParseTaskTest, AnUndoTaskJoinsTheStoredPiecesOfEachItem) {
  Json answer = relocate_task("undo");
  answer["undo"] = {{"not_reversible", ""},
                    {"items", Json::array({Json{{"item", 51}, {"data", "{\"a\":"}},
                                           Json{{"item", 51}, {"data", "1}"}}})}};
  const Result<UserTask> task = parse_task(answer);
  ASSERT_TRUE(task.has_value()) << task.error().message;
  EXPECT_EQ(task->operation, Operation::undo);
  ASSERT_EQ(task->undo.items.size(), 1U);
  EXPECT_EQ(task->undo.items[0].item, PlanItemId{51});
  EXPECT_EQ(task->undo.items[0].data, "{\"a\":1}");
  EXPECT_EQ(task->relocation.kind, placement::CacheKind::pip);
  EXPECT_EQ(task->relocation.content, placement::ContentMode::regenerate);
}

// Cleaning spec, "pnpm store pruned natively": the tray runs the tool's own command once.
TEST_F(UserTaskTest, ANativeCommandRunsOnceAndReportsItsUndoAndVerification) {
  fs().add_file(LR"(D:\pnpm-store\v3\a)", 2_MiB);
  runner().answer("pnpm store path", "D:\\pnpm-store\n");
  runner().answer("pnpm store prune", "removed 10 packages");
  const TaskReport report = run_json(tray_model::testing::native_task_json());
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_EQ(std::ranges::count(runner().calls, std::string("pnpm store prune")), 1);
  EXPECT_EQ(report.undo.not_reversible, "regenerates");
  ASSERT_EQ(report.undo.items.size(), 1U);
  EXPECT_EQ(report.undo.items[0].item, PlanItemId{81}) << "under the plan's item";

  const Json record = report_record(report);
  EXPECT_EQ(record.at("state"), "verified");
  EXPECT_TRUE(record.at("error").is_null());
  EXPECT_EQ(record.at("not_reversible"), "regenerates");
  EXPECT_EQ(record.at("undo"), (Json::array({Json{{"item", 81}, {"data", R"(D:\pnpm-store)"}}})));
  EXPECT_EQ(record.at("verification").at("metric"), "cache_bytes");
  EXPECT_EQ(record.at("verification").at("effect"), "unchanged");
  EXPECT_FALSE(record.contains("skipped"));
  EXPECT_EQ(ipc::check_task_result(record), "");
  EXPECT_EQ(summary(report, "Run `pnpm store prune`"), "Plan 8 (Run `pnpm store prune`): verified");
}

TEST_F(UserTaskTest, AWorseResultThatCannotBeUndoneStaysAppliedWithWhy) {
  fs().add_file(LR"(D:\pnpm-store\v3\a)", 2_MiB);
  runner().answer("pnpm store path", "D:\\pnpm-store\n");
  runner().answer("pnpm store prune", "");
  runner().on_run = [this](const std::string& key) {
    if (key == "pnpm store prune") {
      fs().add_file(LR"(D:\pnpm-store\v3\b)", 1_MiB);
    }
  };
  const TaskReport report = run_json(tray_model::testing::native_task_json());
  EXPECT_EQ(report.state, PlanState::applied);
  EXPECT_EQ(report.error, "verification found cache_bytes worse");
}

// Cleaning spec, "Never as SYSTEM": without a user context the command does not run.
TEST_F(UserTaskTest, ANativeCommandNeverRunsWithoutAUserContext) {
  runner().user = false;
  const TaskReport report = run_json(tray_model::testing::native_task_json());
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_NE(report.error.find("user context is unavailable"), std::string::npos);
  EXPECT_TRUE(runner().calls.empty());
  EXPECT_EQ(report_record(report),
            (Json{{"state", "failed"}, {"error", report.error.substr(0, ipc::max_record_string)}}));
}

TEST_F(UserTaskTest, ACommandOutsideTheAllowListIsRefusedWithoutRunningAnything) {
  Json answer = tray_model::testing::native_task_json();
  answer["action"]["command"] = "cmd /c del C:\\";
  const TaskReport report = run_json(answer);
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_TRUE(runner().calls.empty());
}

TEST_F(UserTaskTest, CancelledBeforeApplyRunsNoPrune) {
  fs().add_file(LR"(D:\pnpm-store\v3\a)", 2_MiB);
  runner().answer("pnpm store path", "D:\\pnpm-store\n");
  std::stop_source quitting;
  quitting.request_stop();
  const TaskReport report = run_json(tray_model::testing::native_task_json(), quitting.get_token());
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_NE(report.error.find("cancelled"), std::string::npos);
  EXPECT_EQ(std::ranges::count(runner().calls, std::string("pnpm store prune")), 0);
}

// Approvals spec, "Approve some": a rejected handler is never purged.
TEST_F(UserTaskTest, OnlyTheHandlersNotRejectedArePurged) {
  host().sizes["Thumbnail Cache"] = Bytes{300};
  host().sizes["Temporary Files"] = Bytes{500};
  const TaskReport report = run_json(handler_task(
      {"Thumbnail Cache", "Temporary Files"},
      {item(41, "handler:Thumbnail Cache", true), item(42, "handler:Temporary Files", false)}));
  EXPECT_EQ(report.state, PlanState::verified) << report.error;
  EXPECT_EQ(host().purged, (std::vector<std::string>{"Thumbnail Cache"}));
  ASSERT_EQ(report.undo.items.size(), 1U);
  EXPECT_EQ(report.undo.items[0].item, PlanItemId{41});
}

TEST_F(UserTaskTest, AHandlerOfTheSystemAreasIsRefusedInTheTray) {
  host().sizes["Update Cleanup"] = Bytes{7000};
  const TaskReport report =
      run_json(handler_task({"Update Cleanup"}, {item(43, "handler:Update Cleanup", true)}));
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_NE(report.error.find("does not run in the tray"), std::string::npos);
  EXPECT_TRUE(host().volumes.empty()) << "not even scanned";
  EXPECT_TRUE(host().purged.empty());
  EXPECT_TRUE(user_handler("Temporary Files"));
  EXPECT_FALSE(user_handler("DownloadsFolder"));
}

TEST_F(UserTaskTest, AFailedPurgeIsReportedWithTheHandlersLeftOut) {
  host().sizes["Thumbnail Cache"] = Bytes{300};
  host().failures.insert_or_assign("Thumbnail Cache",
                                   Error{.code = Errc::refused, .message = "showed a window"});
  const TaskReport report =
      run_json(handler_task({"Thumbnail Cache"}, {item(41, "handler:Thumbnail Cache", true)}));
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_EQ(report.error, "showed a window");
  EXPECT_EQ(
      report_record(report).at("skipped"),
      (Json::array({Json{
          {"target", "Thumbnail Cache"}, {"code", "refused"}, {"message", "showed a window"}}})));
  EXPECT_EQ(ipc::check_task_result(report_record(report)), "");
}

// Placement spec, "Exact Relocation Undo": the tray applies, reports the undo data, and later
// undoes with what the service stored.
TEST_F(UserTaskTest, ARelocationAppliesAndItsReportedUndoRestoresTheVariable) {
  const TaskReport applied = run_json(relocate_task("apply"));
  ASSERT_EQ(applied.state, PlanState::verified) << applied.error;
  EXPECT_EQ(env().value("PIP_CACHE_DIR").value_or(placement::EnvValue{}).text,
            R"(D:\DevCaches\pip)");
  const Json record = report_record(applied);
  EXPECT_FALSE(record.contains("not_reversible"));
  EXPECT_EQ(ipc::check_task_result(record), "");

  // The service stores the pieces and hands them back for the undo.
  Json undo = relocate_task("undo");
  undo["undo"] = {{"not_reversible", ""}, {"items", record.at("undo")}};
  const TaskReport undone = run_json(undo);
  EXPECT_EQ(undone.state, PlanState::rolled_back) << undone.error;
  EXPECT_FALSE(env().value("PIP_CACHE_DIR").has_value()) << "it was not set before";
  EXPECT_EQ(report_record(undone), (Json{{"state", "rolled_back"}, {"error", nullptr}}));
  EXPECT_EQ(ipc::check_task_result(report_record(undone)), "");
}

Json recipe_task(std::string_view operation) {
  const packs::RelocationRecipe recipe{
      .id = "ai-tools/huggingface-home",
      .label = "Hugging Face cache (HF_HOME)",
      .locations = {R"(%USERPROFILE%\.cache\huggingface)"},
      .current = {.kind = packs::SettingKind::env, .text = "HF_HOME"},
      .target_kind = packs::SettingKind::env,
      .content = packs::ContentMode::move,
      .undo = packs::UndoKind::restore};
  return Json{{"plan", 6},
              {"title", R"(Relocate Hugging Face cache (HF_HOME) to D:\Relocated)"},
              {"kind", "relocate"},
              {"operation", operation},
              {"tier", "ask"},
              {"action",
               {{"recipe", placement::recipe_json(recipe)},
                {"instance", ""},
                {"target", R"(D:\Relocated\ai-tools\huggingface-home)"},
                {"content", "move"}}},
              {"items", Json::array({item(61, "relocate:ai-tools/huggingface-home", true)})}};
}

TEST(ParseTaskTest, ReadsARecipeRelocationAndRefusesAMalformedRecipe) {
  const Result<UserTask> task = parse_task(recipe_task("apply"));
  ASSERT_TRUE(task.has_value()) << task.error().message;
  EXPECT_EQ(task->kind, TaskKind::relocate);
  ASSERT_TRUE(task->recipe.has_value());
  const placement::RecipeRelocation recipe = task->recipe.value_or(placement::RecipeRelocation{});
  EXPECT_EQ(recipe.recipe.id, "ai-tools/huggingface-home");
  EXPECT_EQ(recipe.recipe.current.text, "HF_HOME");
  EXPECT_EQ(recipe.target, LR"(D:\Relocated\ai-tools\huggingface-home)");
  EXPECT_EQ(recipe.content, placement::ContentMode::move);

  Json broken = recipe_task("apply");
  broken["action"]["recipe"]["target_kind"] = "junction";
  EXPECT_EQ(parse_task(broken).error().code, Errc::invalid_data);
}

// A pack's recipe runs in the tray like a cache relocation: applied, reported, undone exactly.
TEST_F(UserTaskTest, ARecipeRelocationMovesTheContentAndItsReportedUndoRestoresTheVariable) {
  fs().add_file(LR"(C:\Users\owner\.cache\huggingface\hub\model.safetensors)", 3_GiB);
  const TaskReport applied = run_json(recipe_task("apply"));
  ASSERT_EQ(applied.state, PlanState::verified) << applied.error;
  EXPECT_EQ(env().value("HF_HOME").value_or(placement::EnvValue{}).text,
            R"(D:\Relocated\ai-tools\huggingface-home)");
  EXPECT_TRUE(fs().exists(LR"(D:\Relocated\ai-tools\huggingface-home\hub\model.safetensors)"));
  ASSERT_EQ(applied.undo.items.size(), 1U);
  EXPECT_EQ(applied.undo.items[0].item, PlanItemId{61});
  const Json record = report_record(applied);
  EXPECT_EQ(ipc::check_task_result(record), "");

  Json undo = recipe_task("undo");
  undo["undo"] = {{"not_reversible", ""}, {"items", record.at("undo")}};
  const TaskReport undone = run_json(undo);
  EXPECT_EQ(undone.state, PlanState::rolled_back) << undone.error;
  EXPECT_FALSE(env().value("HF_HOME").has_value()) << "it was not set before";
}

// Placement spec, "Degraded drive excluded": refused at apply time, nothing changed.
TEST_F(UserTaskTest, ARelocationOntoAnUnhealthyDriveIsRefused) {
  drives().at(LR"(D:\)").healthy = false;
  drives().at(LR"(D:\)").health_note = "the health of disk 2 is warning";
  const TaskReport report = run_json(relocate_task("apply"));
  EXPECT_EQ(report.state, PlanState::failed);
  EXPECT_NE(report.error.find("not a safe target"), std::string::npos) << report.error;
  EXPECT_FALSE(env().value("PIP_CACHE_DIR").has_value());
}

TaskReport applied_with(std::string data) {
  return TaskReport{
      .plan = 5,
      .state = PlanState::verified,
      .error = {},
      .undo = Undo{.items = {UndoItem{.item = PlanItemId{51}, .data = std::move(data)}},
                   .not_reversible = {}},
      .verification = std::nullopt,
      .skipped = {}};
}

TEST(ReportRecordTest, LongUndoDataGoesInPiecesThatJoinBackExactly) {
  std::string data;
  for (int index = 0; index < 100; ++index) {
    data += "\xE2\x82\xAC";  // the euro sign: three bytes in UTF-8
  }
  data += std::string(300, 'x');
  const Json record = report_record(applied_with(data));
  std::string joined;
  for (const Json& piece : record.at("undo")) {
    EXPECT_EQ(piece.at("item"), 51);
    const std::string text = piece.at("data").get<std::string>();
    EXPECT_LE(text.size(), ipc::max_record_string);
    joined += text;
  }
  EXPECT_EQ(joined, data);
  EXPECT_EQ(record.at("undo").size(), 3U);
  EXPECT_TRUE(ipc::check_record_bounds(record).empty()) << ipc::check_record_bounds(record);
  EXPECT_EQ(ipc::check_task_result(record), "");
}

TEST(ReportRecordTest, UndoDataTooLargeForARecordIsReportedAsNotReversible) {
  const Json record = report_record(applied_with(std::string(65 * ipc::max_record_string, 'x')));
  EXPECT_FALSE(record.contains("undo"));
  EXPECT_EQ(record.at("not_reversible"), "the undo data is too large to store");
  EXPECT_TRUE(ipc::check_record_bounds(record).empty());
}

TEST(ReportRecordTest, AFailureMessageIsCutAndCleanedForTheRecord) {
  const TaskReport report{.plan = 5,
                          .state = PlanState::failed,
                          .error = "line one\nline two" + std::string(400, 'y'),
                          .undo = {},
                          .verification = std::nullopt,
                          .skipped = {}};
  const Json record = report_record(report);
  const std::string error = record.at("error").get<std::string>();
  EXPECT_EQ(error.size(), ipc::max_record_string);
  EXPECT_TRUE(error.starts_with("line one line two"));
  EXPECT_FALSE(record.contains("undo"));
  EXPECT_TRUE(ipc::check_record_bounds(record).empty());
}

}  // namespace
}  // namespace myclean::user_tasks
