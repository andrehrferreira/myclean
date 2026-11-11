// The Reports screen (core/tray_model/reports.hpp): the stored reports, one report's text and a
// comparison, and the calls the poller makes for them.
#include "core/tray_model/reports.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/screens.hpp"
#include "screens_fixtures.hpp"
#include "tray_model_test_support.hpp"

namespace myclean::tray_model {
namespace {

using testing::FakeLink;
using testing::Json;
using testing::now_seconds;
using testing::reports_list_json;

TEST(ReportsTest, ReadsTheStoredReportsNewestFirst) {
  const Result<std::vector<ReportRow>> reports = parse_reports_list(reports_list_json());
  ASSERT_TRUE(reports.has_value()) << reports.error().message;
  ASSERT_EQ(reports->size(), 2U);
  EXPECT_EQ((*reports)[0].month, "2026-11");
  EXPECT_EQ((*reports)[0].snapshot, "2026-11");
  EXPECT_EQ((*reports)[0].against, "2026-10");
  EXPECT_TRUE((*reports)[1].first_report);
  EXPECT_TRUE((*reports)[1].against.empty());
}

TEST(ReportsTest, ReadsAReportAComparisonAndAMonthWithout) {
  const Result<ReportText> report = parse_reports_get(Json{{"found", true},
                                                           {"month", "2026-11"},
                                                           {"generated_at", now_seconds},
                                                           {"against", "2026-10"},
                                                           {"first_report", false},
                                                           {"markdown", "# November"},
                                                           {"diff", nullptr}});
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_TRUE(report->found);
  EXPECT_EQ(report->title, "Report of 2026-11");
  EXPECT_EQ(report->markdown, "# November");

  const Result<ReportText> missing =
      parse_reports_get(Json{{"found", false},
                             {"month", "2026-09"},
                             {"message", "no report exists for 2026-09"},
                             {"available", {"2026-11", "2026-10"}}});
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->found);
  EXPECT_EQ(missing->available, (std::vector<std::string>{"2026-11", "2026-10"}));

  const Result<ReportText> changes = parse_reports_compare(
      Json{{"found", true}, {"a", "2026-10"}, {"b", "2026-11"}, {"markdown", "## Changes"}});
  ASSERT_TRUE(changes.has_value());
  EXPECT_EQ(changes->title, "Changes from 2026-10 to 2026-11");
}

TEST(ReportsTest, TheScreenListsOpensAndComparesByMonth) {
  EXPECT_EQ(screen_calls(LoadScreen{.screen = Screen::reports}),
            (std::vector<ScreenCall>{{.method = "reports.list", .params = Json::object()}}));
  EXPECT_EQ(screen_calls(LoadScreen{.screen = Screen::reports, .month = "2026-11"}),
            (std::vector<ScreenCall>{{.method = "reports.get", .params = {{"month", "2026-11"}}}}));
  EXPECT_EQ(
      screen_calls(LoadScreen{.screen = Screen::reports, .month = "2026-10", .other = "2026-11"}),
      (std::vector<ScreenCall>{
          {.method = "reports.compare", .params = {{"a", "2026-10"}, {"b", "2026-11"}}}}));

  FakeLink link;
  link.on("metrics.latest", [](const Json&) { return Json(testing::latest_json(now_seconds)); });
  link.on("reports.get", [](const Json&) {
    return Json{{"found", true}, {"month", "2026-11"}, {"markdown", "# November"}};
  });
  Poller poller(link, std::chrono::seconds{600});
  DashboardModel model;
  model.apply(poller.poll(from_unix_seconds(now_seconds), true,
                          {LoadScreen{.screen = Screen::reports, .month = "2026-11"}}));
  const auto& shown = model.screens().report();
  EXPECT_TRUE(shown.has_value() && shown->has_value() && (*shown)->markdown == "# November");
}

}  // namespace
}  // namespace myclean::tray_model
