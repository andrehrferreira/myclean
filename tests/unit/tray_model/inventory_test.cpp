// The Apps, Security and Backups screens (core/tray_model/inventory.hpp): reading their answers,
// the labels that call nothing dangerous without a verdict, and the poller sending an uninstall
// proposal or a Defender scan with the fresh id of the item, over a scripted service.
#include "core/tray_model/inventory.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/screens.hpp"
#include "screens_fixtures.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::apps_json;
using testing::backups_json;
using testing::FakeLink;
using testing::Json;
using testing::now_seconds;
using testing::security_scan_json;

TEST(InventoryTest, ReadsAppsWithTheirLastUseAndVerdict) {
  const Result<AppsList> list = parse_apps_list(apps_json());
  ASSERT_TRUE(list.has_value()) << list.error().message;
  ASSERT_EQ(list->apps.size(), 2U);
  const AppRow& tool = list->apps[0];
  EXPECT_EQ(tool.last_used_source, "Prefetch");
  EXPECT_EQ(tool.days_ago, 200);
  EXPECT_EQ(tool.verdict, "removable");
  EXPECT_TRUE(tool.uninstaller);
  const AppRow& never = list->apps[1];
  EXPECT_FALSE(never.last_used.has_value());
  EXPECT_EQ(never.last_used_text, "no usage recorded in the 40 days of MyClean's history");
  EXPECT_EQ(list->history_days, 40);
  EXPECT_TRUE(unused_for(tool, 180));
  EXPECT_FALSE(unused_for(tool, 365));
  EXPECT_TRUE(unused_for(never, 3650));
  EXPECT_EQ(app_key(tool), app_key(apps_json()["apps"][0]));
  EXPECT_NE(app_key(tool), app_key(never));
}

TEST(InventoryTest, NothingIsCalledDangerousWithoutAVerdict) {
  const Result<SecurityScan> scan = parse_security_scan(security_scan_json());
  ASSERT_TRUE(scan.has_value()) << scan.error().message;
  ASSERT_EQ(scan->items.size(), 3U);
  EXPECT_EQ(trust_label(scan->items[0]), "Windows default");
  EXPECT_EQ(trust_label(scan->items[1]), "suspicious");
  EXPECT_EQ(scan->items[1].virustotal, "VirusTotal does not know this file");
  EXPECT_EQ(trust_label(scan->items[2]), "known dangerous (Microsoft Defender: Trojan:Win32/X)");
  EXPECT_TRUE(known_dangerous(scan->items[2]));

  PersistenceRow claimed = scan->items[1];
  claimed.status = "known_dangerous";  // without a verdict
  EXPECT_FALSE(known_dangerous(claimed));
  EXPECT_EQ(trust_label(claimed), "suspicious");

  EXPECT_EQ(scan->suspicious, 1);
  EXPECT_TRUE(scan->defender_available);
  EXPECT_FALSE(scan->virustotal_enabled);
  EXPECT_EQ(scan->virustotal_note, "off: no security.toml");
  ASSERT_EQ(scan->detections.size(), 1U);
  EXPECT_EQ(persistence_key(scan->items[1]), persistence_key(security_scan_json()["items"][1]));
}

TEST(InventoryTest, ReadsExtensionsAndNetworkTalkers) {
  const Result<Extensions> extensions =
      parse_security_extensions(Json{{"extensions",
                                      {{{"browser", "chrome"},
                                        {"profile", "Default"},
                                        {"id", "abc"},
                                        {"name", "Helper"},
                                        {"version", "1.0"},
                                        {"installed", nullptr},
                                        {"origin", "policy"},
                                        {"enabled", true},
                                        {"flag", "probably not installed by you"}}}},
                                     {"problems", {"firefox: no profile"}}});
  ASSERT_TRUE(extensions.has_value()) << extensions.error().message;
  EXPECT_EQ(extensions->extensions[0].flag, "probably not installed by you");
  EXPECT_EQ(extensions->problems.size(), 1U);

  const Result<NetworkTalkers> talkers =
      parse_security_network(Json{{"hours", 24},
                                  {"from", now_seconds - 86400},
                                  {"to", now_seconds},
                                  {"reverse_dns", false},
                                  {"names_pending", 0},
                                  {"windows_reported", true},
                                  {"processes",
                                   {{{"pid", 4321},
                                     {"created", nullptr},
                                     {"image", "agent.exe"},
                                     {"running", true},
                                     {"session", 1},
                                     {"visible_window", false},
                                     {"trust",
                                      {{"status", "unsigned"},
                                       {"signer", nullptr},
                                       {"unrecognized", true},
                                       {"reasons", {"unsigned", "user-writable-path"}}}},
                                     {"sent", 1000},
                                     {"received", 2000},
                                     {"first_seen", nullptr},
                                     {"last_seen", nullptr},
                                     {"active_minutes", 300},
                                     {"remotes",
                                      {{{"address", "203.0.113.5"},
                                        {"name", nullptr},
                                        {"port", 443},
                                        {"protocol", "tcp"},
                                        {"sent", 1000},
                                        {"received", 2000},
                                        {"active_minutes", 300},
                                        {"open_now", true}}}},
                                     {"listening", {{{"protocol", "tcp"}, {"port", 5000}}}}}}}});
  ASSERT_TRUE(talkers.has_value()) << talkers.error().message;
  const TalkerRow& agent = talkers->processes[0];
  EXPECT_TRUE(agent.unrecognized);
  EXPECT_EQ(agent.reasons.size(), 2U);
  EXPECT_EQ(window_text(agent.visible_window), "no window");
  EXPECT_EQ(window_text(std::nullopt), "unknown");
  EXPECT_TRUE(agent.remotes[0].name.empty());
  EXPECT_EQ(agent.listening, std::vector<std::string>{"tcp 5000"});
}

TEST(InventoryTest, ReadsTheBackupCheckBeforeAndAfterItsFirstRun) {
  const Result<BackupCheck> first =
      parse_backups_check(Json{{"state", "checking"}, {"checked_at", nullptr}, {"error", nullptr}});
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_EQ(first->state, "checking");
  EXPECT_TRUE(first->sources.empty());

  const Result<BackupCheck> check = parse_backups_check(backups_json());
  ASSERT_TRUE(check.has_value()) << check.error().message;
  ASSERT_EQ(check->sources.size(), 2U);
  EXPECT_EQ(backup_status_text(check->sources[1].status), "could not be checked");
  EXPECT_EQ(check->sources[0].extra, std::vector<std::string>{"K:\\Images\\c.mrimg"});
  EXPECT_TRUE(check->backup_exists);
  EXPECT_TRUE(check->has_last_backup);
  EXPECT_EQ(check->last_age_days, 90);
  EXPECT_TRUE(check->recommendation.empty());
  EXPECT_TRUE(check->repositories_checked);
  ASSERT_EQ(check->unprotected.size(), 1U);
  EXPECT_EQ(check->unprotected[0].text, "no remote");

  Json none = backups_json();
  none["backup_exists"] = false;
  none["last_backup"] = nullptr;
  none["recommendation"] = Json{{"text", "Turn on File History to K:."},
                                {"drives", {"K:"}},
                                {"left_out", {{{"drive", "E:"}, {"reason", "USB drive"}}}}};
  const Result<BackupCheck> without = parse_backups_check(none);
  ASSERT_TRUE(without.has_value()) << without.error().message;
  EXPECT_FALSE(without->has_last_backup);
  EXPECT_EQ(without->recommendation, "Turn on File History to K:.");
  EXPECT_EQ(without->left_out, std::vector<std::string>{"E: (USB drive)"});
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

class InventoryPollerTest : public ::testing::Test {
 protected:
  InventoryPollerTest() {
    link_.on("metrics.latest", [](const Json&) { return Json(testing::latest_json(now_seconds)); });
    link_.on("apps.list", [this](const Json&) { return Json(apps_); });
    link_.on("security.scan", [](const Json&) { return Json(security_scan_json()); });
    link_.on("plans.propose_uninstall", [](const Json&) {
      return Json{{"plan", 21},
                  {"state", "awaiting_approval"},
                  {"verdict", "keep"},
                  {"reason", "a runtime"}};
    });
    link_.on("security.defender_scan", [](const Json&) {
      return Json{{"started", true}, {"image", R"(C:\Users\o\AppData\agent.exe)"}};
    });
  }

  PollResult poll(const std::vector<Request>& requests) {
    ++second_;
    return poller_.poll(from_unix_seconds(now_seconds + second_), true, requests);
  }
  FakeLink& link() { return link_; }
  Json& apps() { return apps_; }

 private:
  FakeLink link_;
  Poller poller_{link_, std::chrono::seconds{600}};
  Json apps_ = apps_json();
  std::int64_t second_ = 0;
};

TEST_F(InventoryPollerTest, AnUninstallProposalSendsTheAppsFreshId) {
  DashboardModel model;
  const std::string key = app_key(apps_json()["apps"][1]);
  apps()["apps"][1]["id"] = 9;  // renumbered by another client's list
  model.apply(poll({ScreenAction{.kind = ScreenAction::Kind::uninstall, .keys = {key}}}));
  const std::vector<FakeLink::Call> proposed = link().calls_of("plans.propose_uninstall");
  ASSERT_EQ(proposed.size(), 1U);
  EXPECT_EQ(proposed[0].params, (Json{{"app", 9}}));
  EXPECT_FALSE(outcome(model).failed);
  EXPECT_EQ(outcome(model).text,
            "Plan 21 waits for your approval in Cleanup. MyClean's verdict is keep: a runtime.");
  EXPECT_EQ(parsed(model.screens().apps()).apps[1].id, 9);
}

TEST_F(InventoryPollerTest, ADefenderScanStartsOnlyForTheItemTheOwnerChose) {
  DashboardModel model;
  model.apply(poll({}));
  EXPECT_TRUE(link().calls_of("security.defender_scan").empty());
  const std::string key = persistence_key(security_scan_json()["items"][1]);
  model.apply(poll({ScreenAction{.kind = ScreenAction::Kind::defender_scan, .keys = {key}}}));
  const std::vector<FakeLink::Call> scans = link().calls_of("security.defender_scan");
  ASSERT_EQ(scans.size(), 1U);
  EXPECT_EQ(scans[0].params, (Json{{"item", 2}}));
  EXPECT_EQ(outcome(model).text,
            "Microsoft Defender scans C:\\Users\\o\\AppData\\agent.exe; its verdict shows after "
            "the next Refresh.");
}

TEST_F(InventoryPollerTest, TheScreensLoadTheirMethods) {
  static_cast<void>(
      poll({LoadScreen{.screen = Screen::apps}, LoadScreen{.screen = Screen::security},
            LoadScreen{.screen = Screen::backups, .refresh = true}}));
  EXPECT_EQ(link().calls_of("apps.list").size(), 1U);
  EXPECT_EQ(link().calls_of("security.scan").size(), 1U);
  EXPECT_EQ(link().calls_of("security.extensions").size(), 1U);
  ASSERT_EQ(link().calls_of("security.network").size(), 1U);
  EXPECT_EQ(link().calls_of("security.network")[0].params, (Json{{"hours", 24}}));
  ASSERT_EQ(link().calls_of("backups.check").size(), 1U);
  EXPECT_EQ(link().calls_of("backups.check")[0].params, (Json{{"refresh", true}}));
}

}  // namespace
}  // namespace myclean::tray_model
