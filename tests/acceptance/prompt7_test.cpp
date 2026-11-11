#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "acceptance_fixture.hpp"
#include "core/events/connections.hpp"
#include "core/report/snapshot.hpp"
#include "core/startup/startup.hpp"

// Prompt 7 of docs/prd.md section 7, against the fixture machine of acceptance_fixture.hpp: two
// snapshots one month apart, with a startup item, a service and a listening port added between
// them, and the report of the second month read through get_report and compare_snapshots.
namespace myclean::acceptance {
namespace {

// The "key" of every item `diff` lists as added to, or removed from, set category `category`.
std::vector<std::string> keys(const Json& diff, std::string_view category,
                              std::string_view change) {
  std::vector<std::string> found;
  const Json& set = diff.at("sets").at(category);
  EXPECT_FALSE(set.contains("not_compared")) << category << ": " << set.dump();
  for (const Json& item : set.value(change, Json::array())) {
    found.push_back(item.at("key").get<std::string>());
  }
  return found;
}

// Checks that `diff` adds exactly the Teams Run value, the BackupAgent service and PowerToys'
// listener on 9000, and removes nothing.
void expect_the_three_additions(const Json& diff) {
  EXPECT_EQ(keys(diff, "startup_items", "added"), std::vector<std::string>{"run:user:Teams"});
  EXPECT_EQ(keys(diff, "services", "added"),
            std::vector<std::string>{"service:machine:BackupAgent"});
  const std::vector<std::string> ports = keys(diff, "listening_ports", "added");
  ASSERT_EQ(ports.size(), 1U) << diff.dump();
  EXPECT_TRUE(ports[0].starts_with("tcp/9000/")) << ports[0];
  EXPECT_TRUE(keys(diff, "browser_extensions", "added").empty());
  EXPECT_TRUE(keys(diff, "unsigned_binaries", "added").empty()) << diff.dump();
  for (const std::string_view category : report::set_categories) {
    EXPECT_TRUE(keys(diff, category, "removed").empty()) << category;
  }
}

// Prompt 7, monthly report (RPT-1, RPT-3): "On the 1st of each month: health check, storage
// analysis, intruder scan, then a short report of what changed. Make it a scheduled task." Pass:
// runs on day 1 or next boot; the report diffs against the previous month. Spec "Two consecutive
// reports produce a meaningful diff": the report lists exactly the three additions.
TEST_F(AcceptanceTest, Prompt7ReportsOnTheFirstExactlyWhatChangedSinceTheMonthBefore) {
  sample(10s);
  // October's snapshot and its report, the first one: nothing to compare it with.
  ASSERT_TRUE(produce_report().has_value());
  const Json october = tool("get_report", Json{{"month", "2026-10"}});
  ASSERT_TRUE(october.at("found").get<bool>()) << october.dump();
  EXPECT_TRUE(october.at("first_report").get<bool>());
  EXPECT_TRUE(october.at("diff").is_null());

  // During the month: a startup item, a service and a listening port appear.
  startup_system().snapshot.run.push_back(
      startup::RunValue{.name = "Teams", .command = quoted(teams) + " --system-initiated"});
  startup_system().snapshot.services.push_back(
      startup::Service{.name = "BackupAgent",
                       .display_name = "Backup Agent",
                       .start = {.type = startup::StartType::automatic},
                       .binary = backup_agent.string(),
                       .signer = "Backup Co"});
  std::vector<events::Connection> now_open = connections();
  now_open.push_back({.local_port = 9000, .state = events::TcpState::listen, .pid = 100});
  set_connections(std::move(now_open));

  // The schedule is the 1st at 09:00: nothing runs before.
  sample(20h + 59min);  // 2026-11-01 08:59 UTC
  poll_reports();
  EXPECT_FALSE(tool("get_report", Json{{"month", "2026-11"}}).at("found").get<bool>());
  sample(1min);
  poll_reports();
  const Json november = tool("get_report", Json{{"month", "2026-11"}});
  ASSERT_TRUE(november.at("found").get<bool>()) << november.dump();
  EXPECT_EQ(november.at("against"), "2026-10");
  EXPECT_FALSE(november.at("first_report").get<bool>());
  EXPECT_TRUE(std::filesystem::exists(reports_folder() / "2026-11" / "report.md"));
  EXPECT_TRUE(std::filesystem::exists(reports_folder() / "2026-11" / "report.html"));
  expect_the_three_additions(november.at("diff"));
  // The storage analysis and the intruder scan were compared too, not left out.
  EXPECT_FALSE(november.at("diff").at("top_folders").contains("not_compared"))
      << november.at("diff").dump();
  const std::string markdown = november.at("markdown").get<std::string>();
  for (const char* added : {"Teams", "BackupAgent", "9000"}) {
    EXPECT_NE(markdown.find(added), std::string::npos) << added << "\n" << markdown;
  }

  // Any two snapshots compare the same way; a missing one is named.
  const Json compared = tool("compare_snapshots", Json{{"a", "2026-10"}, {"b", "2026-11"}});
  ASSERT_TRUE(compared.at("found").get<bool>()) << compared.dump();
  expect_the_three_additions(compared.at("diff"));
  const Json missing = tool("compare_snapshots", Json{{"a", "2026-05"}, {"b", "2026-11"}});
  EXPECT_FALSE(missing.at("found").get<bool>());
  EXPECT_EQ(missing.at("missing"), Json::array({"2026-05"}));

  // One report a month: a later poll the same month makes no second one.
  sample(1h);
  poll_reports();
  EXPECT_EQ(other_call("reports.list", Json::object()).value().at("reports").size(), 2U);
  expect_every_action_approved(store());
}

}  // namespace
}  // namespace myclean::acceptance
