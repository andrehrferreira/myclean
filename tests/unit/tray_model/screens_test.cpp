// The screens of the dashboard (core/tray_model/screens.hpp, optimizer.hpp): reading the answers
// of the Placement, Optimize and Startup screens, and the poller loading them and sending the
// owner's proposals, over a scripted service.
#include "core/tray_model/screens.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/optimizer.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "screens_fixtures.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::FakeLink;
using testing::Json;

using testing::now_seconds;
using testing::startup_json;
using testing::system_space_json;

PlanRow plan(std::int64_t id, std::string title, std::string kind, std::string state,
             std::string verification = {}) {
  return PlanRow{.id = id,
                 .title = std::move(title),
                 .kind = std::move(kind),
                 .tier = "ask",
                 .scope = "service",
                 .state = std::move(state),
                 .proposer = "owner",
                 .created_at = from_unix_seconds(now_seconds),
                 .verification = std::move(verification)};
}

TEST(ScreensTest, ReadsTheSystemSettingsWithTheirCostAndAnUnreadableOne) {
  const Result<std::vector<SystemSettingRow>> rows = parse_system_space(system_space_json());
  ASSERT_TRUE(rows.has_value()) << rows.error().message;
  ASSERT_EQ(rows->size(), 3U);
  EXPECT_EQ((*rows)[0].cost, Bytes{54'659'000'000});
  EXPECT_EQ((*rows)[0].recommended, "reduced (about 25.6 GB)");
  EXPECT_FALSE((*rows)[1].recommended.has_value());
  EXPECT_TRUE((*rows)[1].restart_required);
  EXPECT_EQ((*rows)[2].error, "C: has no shadow storage");
  EXPECT_FALSE((*rows)[2].cost.has_value());
  EXPECT_EQ(setting_name("page_file"), "Page file");
  EXPECT_FALSE(parse_system_space(Json{{"settings", {{{"current", "x"}}}}}).has_value());
}

TEST(ScreensTest, RelocationsShowTheNewestPlanOfEachCacheAndTheRecipePlans) {
  const std::vector<PlanRow> plans = {
      plan(9, "Relocate npm to D:\\Relocated\\npm", "relocate", "verified"),
      plan(8, "Relocate wsl/ubuntu to D:\\Relocated", "relocate", "applied"),
      plan(7, "Relocate npm to E:\\Relocated\\npm", "relocate", "rolled_back"),
      plan(6, "Hibernation", "system_setting", "verified", "better")};
  const std::vector<RelocationRow> rows = relocations(plans);
  ASSERT_EQ(rows.size(), relocatable_caches.size() + 1);
  const auto npm = std::ranges::find(rows, std::string("npm"), &RelocationRow::cache);
  ASSERT_NE(npm, rows.end());
  ASSERT_NE(npm->plan, nullptr);
  EXPECT_EQ(npm->plan->id, 9);
  EXPECT_EQ(rows.front().cache, "temp");
  EXPECT_EQ(rows.front().plan, nullptr);
  EXPECT_TRUE(rows.back().cache.empty());
  EXPECT_EQ(rows.back().plan->id, 8);
}

TEST(ScreensTest, AppliedOptimizationsCarryTheirVerification) {
  const std::vector<PlanRow> plans = {
      plan(5, "Disable 2 startup items", "system_setting", "applied", "pending"),
      plan(4, "Compact 1 disk", "compact", "awaiting_approval"),
      plan(3, "Clean caches", "cleanup", "verified", "better"),
      plan(2, "Page file", "system_setting", "rolled_back", "worse")};
  const std::vector<const PlanRow*> applied = applied_optimizations(plans);
  ASSERT_EQ(applied.size(), 2U);
  EXPECT_EQ(applied[0]->id, 5);
  EXPECT_EQ(applied[1]->id, 2);
  EXPECT_EQ(verification_text("pending"), "measured again later");
  EXPECT_EQ(verification_text("worse"), "worse");
  EXPECT_EQ(verification_text(""), "not measured yet");
}

TEST(ScreensTest, ReadsStartupItemsTheirImpactAndWhatATickCanChange) {
  const Result<std::vector<StartupItemRow>> items = parse_startup_list(startup_json());
  ASSERT_TRUE(items.has_value()) << items.error().message;
  ASSERT_EQ(items->size(), 3U);
  EXPECT_EQ(impact_text((*items)[0].impact),
            "1.2 s CPU, 35.0 MB of disk in the first 2 minutes (median of 5 logons)");
  EXPECT_EQ(impact_text((*items)[1].impact), "delays the boot by 3.4 s (Windows)");
  EXPECT_EQ(impact_text((*items)[2].impact), "not measured yet");
  EXPECT_FALSE((*items)[2].enabled.has_value());
  EXPECT_EQ(startup_source_text("folder"), "Startup folder");

  const std::set<std::string> all = {"run:user:Discord", "run:user:Steam", "runonce:user:Setup"};
  EXPECT_EQ(startup_choice(*items, all, false).value(),
            std::vector<std::string>{"run:user:Discord"});
  EXPECT_EQ(startup_choice(*items, all, true).value(), std::vector<std::string>{"run:user:Steam"});
  EXPECT_FALSE(startup_choice(*items, {"runonce:user:Setup"}, false).has_value());
}

TEST(ScreensTest, ReadsBootTimesWithContributorsAndChanges) {
  const Json reply{
      {"source", "diagnostics"},
      {"log_error", nullptr},
      {"latest",
       {{"start", now_seconds},
        {"boot_seconds", 41.5},
        {"main_path_seconds", 20.0},
        {"post_boot_seconds", 21.5},
        {"logon_seconds", 30.0},
        {"boot_difference_seconds", 2.5},
        {"logon_difference_seconds", nullptr},
        {"logon", now_seconds + 60},
        {"contributors",
         {{{"image", "discord.exe"},
           {"cpu_seconds", 3.0},
           {"io_bytes", 1000},
           {"item", {{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}},
          {{"image", "svchost.exe"}, {"cpu_seconds", 1.0}, {"io_bytes", 10}, {"item", nullptr}}}}}},
      {"median", {{"boot_seconds", 39.0}, {"logon_seconds", 28.0}, {"boots", 12}}},
      {"changes",
       {{{"plan", 4},
         {"applied", now_seconds - 86400},
         {"items", {{{"name", "Discord"}, {"source", "run"}, {"scope", "user"}}}},
         {"before", {{"logon_seconds", 35.0}, {"boots", 5}}},
         {"after", nullptr}}}}};
  const Result<BootTimes> times = parse_boot_times(reply);
  ASSERT_TRUE(times.has_value()) << times.error().message;
  EXPECT_DOUBLE_EQ(times->boot_seconds, 41.5);
  EXPECT_EQ(times->boot_difference, 2.5);
  EXPECT_FALSE(times->logon_difference.has_value());
  EXPECT_EQ(times->median_boots, 12);
  ASSERT_EQ(times->contributors.size(), 2U);
  EXPECT_EQ(times->contributors[0].item, "Discord");
  EXPECT_TRUE(times->contributors[1].item.empty());
  ASSERT_EQ(times->changes.size(), 1U);
  EXPECT_EQ(times->changes[0].logon_before, 35.0);
  EXPECT_FALSE(times->changes[0].logon_after.has_value());

  const Result<BootTimes> none = parse_boot_times(Json{{"source", "diagnostics"},
                                                       {"log_error", "access denied"},
                                                       {"latest", nullptr},
                                                       {"median", nullptr},
                                                       {"changes", Json::array()}});
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->log_error, "access denied");
  EXPECT_FALSE(none->latest_start.has_value());
}

TEST(ScreensTest, ReadsTheOptimizeAnalysesAlsoBeforeTheirFirstResult) {
  const Result<VirtualDisks> first =
      parse_vhdx(Json{{"state", "scanning"}, {"scanned_at", nullptr}, {"error", nullptr}});
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_EQ(first->analysis.state, "scanning");
  EXPECT_TRUE(first->disks.empty());

  const Result<VirtualDisks> disks =
      parse_vhdx(Json{{"state", "ready"},
                      {"scanned_at", now_seconds},
                      {"error", nullptr},
                      {"disks",
                       {{{"id", 31},
                         {"path", "D:\\WSL\\ext4.vhdx"},
                         {"area", "wsl/ubuntu"},
                         {"label", "WSL Ubuntu"},
                         {"distribution", "Ubuntu"},
                         {"size", 80'000},
                         {"size_on_disk", 70'000},
                         {"in_use", true}}}},
                      {"total", 80'000},
                      {"notes", {"Docker Desktop is not installed"}}});
  ASSERT_TRUE(disks.has_value()) << disks.error().message;
  ASSERT_EQ(disks->disks.size(), 1U);
  EXPECT_EQ(disks->disks[0].id, 31);
  EXPECT_TRUE(disks->disks[0].in_use);
  EXPECT_EQ(disks->notes.size(), 1U);

  const Result<IndexedFolders> folders = parse_indexed_folders(Json{
      {"state", "failed"}, {"scanned_at", nullptr}, {"error", "Windows Search is not running"}});
  ASSERT_TRUE(folders.has_value());
  EXPECT_EQ(folders->analysis.error, "Windows Search is not running");

  const Result<DevDriveAdvice> advice = parse_dev_drive(
      Json{{"state", "ready"},
           {"scanned_at", now_seconds},
           {"error", nullptr},
           {"supported", true},
           {"build", "26200.7019"},
           {"dev_drives", Json::array()},
           {"explanation", "A Dev Drive is a ReFS volume."},
           {"repositories", {{{"path", "F:\\src\\a"}, {"bytes", 10}, {"changed", now_seconds}}}},
           {"caches", Json::array()},
           {"total_bytes", 10},
           {"recommended_bytes", 50'000'000'000},
           {"host", {{"volume", "D:"}, {"free", 1'000'000'000'000}, {"drive_class", "nvme"}}},
           {"steps", {"Create the Dev Drive", "Move the repositories"}},
           {"notes", Json::array()},
           {"sources", Json::array()}});
  ASSERT_TRUE(advice.has_value()) << advice.error().message;
  EXPECT_EQ(advice->repositories, 1);
  EXPECT_EQ(advice->host, "D: (nvme, 931 GB free)");
  EXPECT_EQ(advice->steps.size(), 2U);

  const Result<std::vector<ResidentCostRow>> costs =
      parse_resident_costs(Json{{"days", 30},
                                {"from", now_seconds},
                                {"programs",
                                 {{{"image", "code.exe"},
                                   {"key", "code.exe"},
                                   {"hours_running", 200.0},
                                   {"cpu_seconds", 9000.0},
                                   {"ram_average", 1000},
                                   {"ram_peak", 2000},
                                   {"disk_bytes", 3000},
                                   {"net_bytes", 4000}}}}});
  ASSERT_TRUE(costs.has_value()) << costs.error().message;
  EXPECT_EQ((*costs)[0].network, Bytes{4000});
}

// The parsed answer of a screen method; throws (failing the test) when there is none.
template <typename T>
const T& parsed(const Answer<T>& answer) {
  if (!answer || !answer->has_value()) {
    throw std::logic_error("no parsed answer");
  }
  return **answer;
}

const ActionOutcome& outcome(const DashboardModel& model) {
  const std::optional<ActionOutcome>& action = model.screens().action();
  if (!action) {
    throw std::logic_error("no outcome");
  }
  return *action;
}

// A scripted service with the screens' methods.
class ScreensPollerTest : public ::testing::Test {
 protected:
  ScreensPollerTest() {
    link_.on("metrics.latest", [](const Json&) { return Json(testing::latest_json(now_seconds)); });
    link_.on("startup.list", [this](const Json&) { return Json(startup_); });
    link_.on("startup.boot_times", [](const Json&) {
      return Json{{"source", "diagnostics"},
                  {"log_error", nullptr},
                  {"latest", nullptr},
                  {"median", nullptr},
                  {"changes", Json::array()}};
    });
    link_.on("plans.propose_startup",
             [](const Json&) { return Json{{"plan", 12}, {"state", "awaiting_approval"}}; });
    link_.on("space.vhdx", [this](const Json&) {
      return Json{{"state", vhdx_state_}, {"scanned_at", nullptr}, {"error", nullptr}};
    });
  }

  PollResult poll(const std::vector<Request>& requests = {}, bool open = true) {
    ++second_;
    return poller_.poll(from_unix_seconds(now_seconds + second_), open, requests);
  }

  FakeLink& link() { return link_; }
  Json& startup() { return startup_; }
  void set_vhdx_state(std::string state) { vhdx_state_ = std::move(state); }

 private:
  FakeLink link_;
  Poller poller_{link_, std::chrono::seconds{600}};
  Json startup_ = startup_json();
  std::string vhdx_state_ = "scanning";
  std::int64_t second_ = 0;
};

TEST_F(ScreensPollerTest, OpeningAScreenLoadsItsAnswersOnce) {
  DashboardModel model;
  model.apply(poll({LoadScreen{.screen = Screen::startup}}));
  EXPECT_EQ(parsed(model.screens().startup_items()).size(), 3U);
  ASSERT_TRUE(model.screens().boot_times().has_value());
  EXPECT_FALSE(model.screens().system_space().has_value());  // not asked for

  link().clear_calls();
  model.apply(poll());
  EXPECT_TRUE(link().calls_of("startup.list").empty());
  EXPECT_TRUE(model.screens().startup_items().has_value());  // kept
}

TEST_F(ScreensPollerTest, AnAnalysisThatRunsIsAskedForAgainUntilReadyWithoutARefresh) {
  DashboardModel model;
  model.apply(poll({LoadScreen{.screen = Screen::optimize, .refresh = true}}));
  ASSERT_EQ(link().calls_of("space.vhdx").size(), 1U);
  EXPECT_EQ(link().calls_of("space.vhdx")[0].params, (Json{{"refresh", true}}));
  // The other methods of the screen are not offered by this service: shown as failed.
  const auto& costs = model.screens().resident_costs();
  EXPECT_TRUE(costs.has_value() && !costs->has_value());

  link().clear_calls();
  model.apply(poll());
  ASSERT_EQ(link().calls_of("space.vhdx").size(), 1U);
  EXPECT_EQ(link().calls_of("space.vhdx")[0].params, Json::object());
  EXPECT_TRUE(link().calls_of("space.dev_drive").empty());

  set_vhdx_state("ready");
  model.apply(poll());
  EXPECT_EQ(parsed(model.screens().virtual_disks()).analysis.state, "ready");
  link().clear_calls();
  model.apply(poll());
  EXPECT_TRUE(link().calls_of("space.vhdx").empty());
}

TEST_F(ScreensPollerTest, ClosingTheDashboardStopsAskingAgain) {
  static_cast<void>(poll({LoadScreen{.screen = Screen::optimize}}));
  static_cast<void>(poll({}, false));
  link().clear_calls();
  static_cast<void>(poll());
  EXPECT_TRUE(link().calls_of("space.vhdx").empty());
}

TEST_F(ScreensPollerTest, AStartupProposalSendsTheFreshIdsOfItsItems) {
  DashboardModel model;
  // Another client's list moved Discord to number 2 meanwhile.
  startup()["items"][0]["id"] = 2;
  startup()["items"][1]["id"] = 1;
  model.apply(poll({ScreenAction{
      .kind = ScreenAction::Kind::startup, .keys = {"run:user:Discord"}, .enable = false}}));
  const std::vector<FakeLink::Call> proposed = link().calls_of("plans.propose_startup");
  ASSERT_EQ(proposed.size(), 1U);
  EXPECT_EQ(proposed[0].params, (Json{{"candidates", {{"ids", {2}}}}, {"enable", false}}));
  EXPECT_FALSE(outcome(model).failed);
  EXPECT_EQ(outcome(model).text, "Plan 12 waits for your approval in Cleanup.");
  EXPECT_EQ(parsed(model.screens().startup_items()).front().id,
            2);                                         // the fresh list is shown
  EXPECT_FALSE(link().calls_of("plans.list").empty());  // and the inbox fetched
}

TEST_F(ScreensPollerTest, AProposalOfAnItemGoneSinceSendsNothing) {
  DashboardModel model;
  model.apply(poll({ScreenAction{
      .kind = ScreenAction::Kind::startup, .keys = {"run:user:Gone"}, .enable = true}}));
  EXPECT_TRUE(link().calls_of("plans.propose_startup").empty());
  EXPECT_TRUE(outcome(model).failed);
  EXPECT_EQ(outcome(model).text,
            "Proposing the startup change failed: run:user:Gone is no longer listed");
}

TEST(ScreensTest, ProposalsNameOnlyTokensAndIntegerIds) {
  EXPECT_EQ(
      action_call(ScreenAction{.kind = ScreenAction::Kind::system_setting, .value = "hibernation"},
                  {}),
      (ScreenCall{"plans.propose_optimization", Json{{"kind", "hibernation"}}}));
  EXPECT_EQ(
      action_call(ScreenAction{.kind = ScreenAction::Kind::relocation, .value = "npm"}, {}),
      (ScreenCall{"plans.propose_optimization", Json{{"kind", "relocate"}, {"cache", "npm"}}}));
  EXPECT_EQ(action_call(ScreenAction{.kind = ScreenAction::Kind::compact}, {4, 5}),
            (ScreenCall{"plans.propose_compact", Json{{"candidates", {{"ids", {4, 5}}}}}}));
  EXPECT_EQ(action_call(ScreenAction{.kind = ScreenAction::Kind::index_exclusion}, {7}),
            (ScreenCall{"plans.propose_index_exclusion", Json{{"candidates", {{"ids", {7}}}}}}));
  EXPECT_FALSE(list_call(ScreenAction{.kind = ScreenAction::Kind::compact}).has_value());
}

}  // namespace
}  // namespace myclean::tray_model
