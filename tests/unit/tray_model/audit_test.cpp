// The Audit and Settings screens (core/tray_model/audit.hpp): one page of the action log, its
// paging through the poller, and the snoozed alerts of the Settings screen.
#include "core/tray_model/audit.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/tray_model/inventory.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/screens.hpp"
#include "core/tray_model/snapshot.hpp"
#include "screens_fixtures.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::audit_json;
using testing::FakeLink;
using testing::Json;
using testing::now_seconds;

TEST(AuditTest, ReadsAPageOfTheLogNewestFirst) {
  const Result<AuditPage> page = parse_audit_list(audit_json());
  ASSERT_TRUE(page.has_value()) << page.error().message;
  ASSERT_EQ(page->entries.size(), 2U);
  EXPECT_EQ(page->next, 41);
  EXPECT_EQ(audit_text(page->entries[0]), "awaiting_approval -> approved: accepted");
  EXPECT_EQ(audit_text(page->entries[1]), "item 3: deleted (freed=4096)");
  EXPECT_EQ(page->entries[1].plan, 11);

  const Result<AuditPage> last =
      parse_audit_list(Json{{"entries", Json::array()}, {"next", nullptr}});
  ASSERT_TRUE(last.has_value());
  EXPECT_FALSE(last->next.has_value());
  EXPECT_FALSE(parse_audit_list(Json{{"entries", {{{"id", 1}}}}}).has_value());
}

TEST(AuditTest, ThePollerAsksForThePageBelowTheOneShown) {
  FakeLink link;
  link.on("metrics.latest", [](const Json&) { return Json(testing::latest_json(now_seconds)); });
  link.on("audit.list", [](const Json&) { return Json(audit_json()); });
  Poller poller(link, std::chrono::seconds{600});
  static_cast<void>(
      poller.poll(from_unix_seconds(now_seconds), true, {LoadScreen{.screen = Screen::audit}}));
  static_cast<void>(poller.poll(from_unix_seconds(now_seconds + 1), true,
                                {LoadScreen{.screen = Screen::audit, .before = 41}}));
  const std::vector<FakeLink::Call> calls = link.calls_of("audit.list");
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].params, Json::object());
  EXPECT_EQ(calls[1].params, (Json{{"before", 41}}));
}

TEST(AuditTest, SettingsListOnlyTheAlertsStillSnoozed) {
  const WallTime now = from_unix_seconds(now_seconds);
  std::vector<AlertRow> alerts(3);
  alerts[0].rule = "volume.fill-up";
  alerts[0].snoozed_until = now + std::chrono::hours{1};
  alerts[1].rule = "disk.temperature";
  alerts[1].snoozed_until = now - std::chrono::hours{1};
  alerts[2].rule = "backup.age";
  const std::vector<const AlertRow*> snoozed = snoozed_alerts(alerts, now);
  ASSERT_EQ(snoozed.size(), 1U);
  EXPECT_EQ(snoozed[0]->rule, "volume.fill-up");
}

TEST(AuditTest, TheSettingsScreenReadsVirusTotalAndTheWorkspaceRoots) {
  EXPECT_EQ(screen_calls(LoadScreen{.screen = Screen::settings}),
            (std::vector<ScreenCall>{{.method = "security.scan", .params = Json::object()},
                                     {.method = "backups.check", .params = Json::object()}}));
  Json check = testing::backups_json();
  check["repositories"]["roots"] = {"F:\\", "W:\\"};
  const Result<BackupCheck> parsed = parse_backups_check(check);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->workspace_roots, (std::vector<std::string>{"F:\\", "W:\\"}));
}

}  // namespace
}  // namespace myclean::tray_model
