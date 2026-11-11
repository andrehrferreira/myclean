#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/actions/schema.hpp"
#include "core/governing/records.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/report/diff.hpp"
#include "core/report/month.hpp"
#include "core/report/render.hpp"
#include "core/report/repository.hpp"
#include "core/report/snapshot.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "report_fakes.hpp"

namespace myclean::report {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using testing::FakeService;

const WallTime first_of_november = std::chrono::sys_days{std::chrono::November / 1 / 2026} + 9h;

const std::chrono::time_zone& utc() {
  return *std::chrono::locate_zone("UTC");
}

// A store with the tables the snapshot counts from, in a folder of this test.
class History {
 public:
  explicit History(const os::IClock& clock) {
    const ::testing::TestInfo* test = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("myclean-report-" + std::string(test->test_suite_name()) + "-" + test->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    const std::vector<store::MigrationSet> sets = {
        governing::governor_migrations(), input::input_migrations(), actions::action_migrations(),
        report_migrations()};
    store_ = store::Store::open(dir_ / "myclean.db", clock, sets).value();
  }
  History(const History&) = delete;
  History& operator=(const History&) = delete;
  History(History&&) = delete;
  History& operator=(History&&) = delete;
  ~History() {
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  void exec(std::string_view sql) {
    ASSERT_TRUE(store_->write([&](store::Database& db) { return db.exec(sql); }).has_value());
  }
  [[nodiscard]] store::Store& store() { return *store_; }

 private:
  std::filesystem::path dir_;
  std::unique_ptr<store::Store> store_;
};

Probes probes_of(FakeService& service, store::Store* store) {
  return Probes{
      .call = service.caller(), .wait = {}, .store = store, .poll_interval = 0s, .max_polls = 5};
}

std::int64_t seconds(WallTime time) {
  return store::to_unix_seconds(time);
}

// Month helpers ----------------------------------------------------------------------------------

TEST(ReportMonthTest, MonthsAreYyyyMmTokens) {
  const std::optional<std::chrono::year_month> month = parse_month("2026-11");
  ASSERT_TRUE(month.has_value());
  EXPECT_EQ(*month, std::chrono::year{2026} / std::chrono::November);
  EXPECT_EQ(month_text(*month), "2026-11");
  for (const std::string_view bad :
       {"2026-13", "2026-00", "26-11", "2026/11", "2026-1", "", "May"}) {
    EXPECT_FALSE(parse_month(bad).has_value()) << bad;
  }
  EXPECT_EQ(month_of(first_of_november - 10h, utc()),
            std::chrono::year{2026} / std::chrono::October);
  // 00:30 on the 1st in Sao Paulo is still October 31st in UTC terms, but November locally.
  const std::chrono::time_zone& sao_paulo = *std::chrono::locate_zone("America/Sao_Paulo");
  EXPECT_EQ(
      month_of(std::chrono::sys_days{std::chrono::November / 1 / 2026} + 3h + 30min, sao_paulo),
      std::chrono::year{2026} / std::chrono::November);
  EXPECT_EQ(html_file("R", *month), std::filesystem::path("R") / "2026-11" / "report.html");
  EXPECT_EQ(markdown_file("R", *month), std::filesystem::path("R") / "2026-11" / "report.md");
}

// Reports spec, "No duplicate run in the same month" and the interrupted run of the same month.
TEST(ReportMonthTest, ARunOfThisMonthIsRetriedAtStartOnlyWithoutASnapshot) {
  const WallTime later = first_of_november + 26h;
  EXPECT_TRUE(retry_at_start(first_of_november, false, later, utc()));
  EXPECT_FALSE(retry_at_start(first_of_november, true, later, utc()));
  EXPECT_FALSE(retry_at_start(first_of_november - 24h * 30, false, later, utc()));
  EXPECT_FALSE(retry_at_start(std::nullopt, false, later, utc()));
}

// Snapshot ---------------------------------------------------------------------------------------

// Reports spec, "Snapshot is complete": every category is there, collected or not collected with
// the reason, never empty.
TEST(ReportSnapshotTest, EveryCategoryIsCollectedOrSaysWhyNot) {
  const fakes::FakeClock clock(first_of_november);
  History history(clock);
  const std::int64_t now = seconds(first_of_november);
  history.exec(
      "INSERT INTO governor_event(time, kind, step, reason, actor, detail) VALUES(" +
      std::to_string(now - 100) + ", 'apply_step', 'priority', 'cpu', 'governor', ''), (" +
      std::to_string(now - 90) + ", 'apply_step', 'ecoqos', 'cpu', 'governor', 'observe'), (" +
      std::to_string(now - 80) + ", 'roll_back', 'priority', 'pressure_cleared', 'governor', '')");
  history.exec(
      "INSERT INTO input_incident(time, chord, latency_ms, capped, input_lag_ms, formats, "
      "hung, suspects) VALUES(" +
      std::to_string(now - 50) + ", 'ctrl+v', 900, 0, 10, '[]', '[]', '[]')");
  history.exec(
      "INSERT INTO action_log(time, plan_id, item_id, actor, event, outcome, metrics) "
      "VALUES(" +
      std::to_string(now - 40) + ", 1, 1, 'scheduler', 'apply', 'removed', 'freed=4096'), (" +
      std::to_string(now - 40) + ", 2, 5, 'owner', 'apply', 'removed', 'freed=999')");

  FakeService service;
  service.fail_method("security.extensions", "the owner's profiles cannot be read");
  const Json snapshot = take_snapshot(probes_of(service, &history.store()), first_of_november,
                                      first_of_november - 24h * 30, utc());

  EXPECT_EQ(snapshot.at("month"), "2026-11");
  EXPECT_EQ(snapshot.at("taken_at"), now);
  const Json& categories_of = snapshot.at("categories");
  ASSERT_EQ(categories_of.size(), categories.size());
  for (const std::string_view name : categories) {
    const Json found = category(snapshot, name);
    if (name == "browser_extensions") {
      EXPECT_EQ(found.at("status"), "not_collected");
      EXPECT_EQ(found.at("reason"), "security.extensions: the owner's profiles cannot be read");
    } else {
      EXPECT_TRUE(collected(found)) << name << ": " << found.dump();
    }
  }
  const Json startup = category(snapshot, "startup_items").at("items");
  ASSERT_EQ(startup.size(), 1U);
  EXPECT_EQ(startup[0].at("key"), "run:user:Discord");
  EXPECT_EQ(category(snapshot, "services").at("items")[0].at("display_name"), "Acme Service");
  EXPECT_EQ(category(snapshot, "listening_ports").at("items")[0].at("key"), "tcp/135/svchost.exe");
  const Json unsigned_items = category(snapshot, "unsigned_binaries").at("items");
  ASSERT_EQ(unsigned_items.size(), 1U);
  EXPECT_EQ(unsigned_items[0].at("path"), R"(C:\Users\owner\AppData\up.exe)");
  EXPECT_EQ(unsigned_items[0].at("seen_as"), Json::array({"persistence"}));
  EXPECT_EQ(category(snapshot, "top_folders").at("folders").size(), 2U);
  EXPECT_EQ(category(snapshot, "cleanup_candidates").at("duplicate_photos"),
            (Json{{"count", 2}, {"bytes", 500}}));
  EXPECT_EQ(category(snapshot, "free_space").at("change"), -10'000'000'000);
  EXPECT_EQ(category(snapshot, "governor").at("interventions"), 2);
  EXPECT_EQ(category(snapshot, "governor").at("recorded_only"), 1);
  EXPECT_EQ(category(snapshot, "freezes").at("incidents"), 1);
  EXPECT_EQ(category(snapshot, "space_freed").at("bytes"), 4096);  // the owner's plan is not auto
  // The security part still has the persistence points without the extensions.
  EXPECT_EQ(category(snapshot, "security").at("suspicious").size(), 1U);
  EXPECT_TRUE(category(snapshot, "security").at("flagged_extensions").is_null());
}

TEST(ReportSnapshotTest, WithoutTheHistoryStoreTheCountsAreNotCollected) {
  FakeService service;
  const Json snapshot = take_snapshot(probes_of(service, nullptr), first_of_november,
                                      first_of_november - 24h * 30, utc());
  for (const std::string_view name : {"governor", "freezes", "space_freed"}) {
    EXPECT_EQ(category(snapshot, name).at("reason"), "the history store is not available") << name;
  }
}

// Reports spec, "Health uses 30-day aggregates": the build tree is idle now (processes.top) but
// averaged 40 % CPU over the last 30 days, and the report lists that average.
TEST(ReportSnapshotTest, HealthUsesThirtyDayAggregatesNotTheCurrentSample) {
  FakeService service;
  const Json snapshot = take_snapshot(probes_of(service, nullptr), first_of_november,
                                      first_of_november - 24h * 30, utc());
  const Json health = category(snapshot, "health");
  ASSERT_TRUE(collected(health));
  EXPECT_EQ(health.at("days"), 30);
  ASSERT_EQ(health.at("programs").size(), 1U);
  EXPECT_EQ(health.at("programs")[0].at("image"), "cl.exe");
  EXPECT_NEAR(health.at("programs")[0].at("cpu_percent").get<double>(), 40.0, 1e-9);
  EXPECT_EQ(health.at("cpu"), (Json{{"mean", 22.0}, {"max", 95.0}}));
  bool asked_for_the_month = false;
  for (const auto& [method, params] : service.calls()) {
    if (method == "metrics.query" && params.at("metric") == "cpu.utility") {
      asked_for_the_month = params.at("max_points") == 1 &&
                            params.at("from") == seconds(first_of_november - 24h * 30);
    }
  }
  EXPECT_TRUE(asked_for_the_month);
}

TEST(ReportSnapshotTest, StorageAnalysesAreAskedAgainUntilReadyOrGivenUp) {
  FakeService service;
  service.keep_scanning(3);
  Json snapshot = take_snapshot(probes_of(service, nullptr), first_of_november,
                                first_of_november - 24h * 30, utc());
  EXPECT_TRUE(collected(category(snapshot, "top_folders")));
  EXPECT_EQ(category(snapshot, "cleanup_candidates").at("old_downloads").at("count"), 2);

  FakeService slow;
  slow.keep_scanning(100);
  snapshot = take_snapshot(probes_of(slow, nullptr), first_of_november,
                           first_of_november - 24h * 30, utc());
  EXPECT_EQ(category(snapshot, "top_folders").at("reason"),
            "space.top: still running after 5 polls");

  FakeService stopped;
  stopped.keep_scanning(100);
  Probes probes = probes_of(stopped, nullptr);
  probes.wait = [](std::chrono::seconds) { return false; };
  snapshot = take_snapshot(probes, first_of_november, first_of_november - 24h * 30, utc());
  EXPECT_EQ(category(snapshot, "top_folders").at("reason"),
            "space.top: the monthly job stopped first");
  EXPECT_EQ(category(snapshot, "cleanup_candidates").at("used_installers").at("error"),
            "space.used_installers: the monthly job stopped first");
}

// Diff -------------------------------------------------------------------------------------------

Json snapshot_of(FakeService& service, WallTime when) {
  return take_snapshot(probes_of(service, nullptr), when, when - 24h * 30, utc());
}

// Reports spec, "New extension and listening port appear in the diff".
TEST(ReportDiffTest, ANewExtensionAndListeningPortAreListedAsNew) {
  FakeService service;
  const Json before = snapshot_of(service, first_of_november - 24h * 30);
  service.answer("security.extensions")
      .at("extensions")
      .push_back(FakeService::extension("abc", "Coupon Helper", "probably not installed by you"));
  service.answer("security.network")
      .at("processes")
      .push_back(Json{{"pid", 4242},
                      {"image", "node.exe"},
                      {"visible_window", false},
                      {"sent", 0},
                      {"received", 0},
                      {"listening", Json::array({{{"protocol", "tcp"}, {"port", 8080}}})}});
  service.answer("startup.list").at("items").erase(0);  // Discord no longer starts
  service.answer("space.top").at("folders")[0].at("size_on_disk") = 60'000'000'000;
  service.answer("disks.health").at("drives")[0] = FakeService::drive("warning", 2);
  const Json after = snapshot_of(service, first_of_november);

  const Json changes = diff(before, after);
  EXPECT_EQ(changes.at("from"), "2026-10");
  EXPECT_EQ(changes.at("to"), "2026-11");
  const Json extensions = changes.at("sets").at("browser_extensions");
  ASSERT_EQ(extensions.at("added").size(), 1U);
  EXPECT_EQ(extensions.at("added")[0].at("id"), "abc");
  EXPECT_TRUE(extensions.at("removed").empty());
  const Json ports = changes.at("sets").at("listening_ports");
  ASSERT_EQ(ports.at("added").size(), 1U);
  EXPECT_EQ(ports.at("added")[0].at("port"), 8080);
  EXPECT_EQ(ports.at("added")[0].at("image"), "node.exe");
  EXPECT_EQ(changes.at("sets").at("startup_items").at("removed")[0].at("name"), "Discord");
  EXPECT_TRUE(changes.at("sets").at("services").at("added").empty());
  EXPECT_EQ(changes.at("top_folders").at("grown")[0].at("change"), 10'000'000'000);
  const Json drive = changes.at("disk_health")[0];
  EXPECT_EQ(drive.at("status_before"), "good");
  EXPECT_EQ(drive.at("status_after"), "warning");
  EXPECT_EQ(drive.at("changes").at("media_errors"), (Json{{"before", 0}, {"after", 2}}));
  EXPECT_EQ(changes.at("counts").at("governor_interventions"),
            (Json{{"before", nullptr}, {"after", nullptr}}));

  const std::string text = render(after, changes, parse_month("2026-10"), utc()).markdown;
  EXPECT_NE(text.find("New: Coupon Helper (abc) in chrome/Default, from store, probably not "
                      "installed by you"),
            std::string::npos);
  EXPECT_NE(text.find("New: TCP 8080 by node.exe (pid 4242)"), std::string::npos);
  EXPECT_NE(text.find("Gone: Discord (run, user): Discord.exe --start"), std::string::npos);
}

TEST(ReportDiffTest, ACategoryNotCollectedOnEitherSideIsNotCompared) {
  FakeService service;
  const Json before = snapshot_of(service, first_of_november - 24h * 30);
  service.fail_method("startup.list", "access denied");
  const Json after = snapshot_of(service, first_of_november);
  const Json changes = diff(before, after);
  EXPECT_EQ(changes.at("sets").at("startup_items").at("not_compared"),
            "not collected in 2026-11: startup.list: access denied");
  EXPECT_TRUE(changes.at("sets").at("browser_extensions").at("added").empty());
}

// Rendering --------------------------------------------------------------------------------------

// Reports spec, "First-ever report".
TEST(ReportRenderTest, TheFirstReportSaysSoAndListsTheBaseline) {
  FakeService service;
  const Json snapshot = snapshot_of(service, first_of_november);
  const Rendered page = render(snapshot, Json(nullptr), std::nullopt, utc());
  EXPECT_NE(page.markdown.find("First report, no previous snapshot to compare"), std::string::npos);
  EXPECT_EQ(page.markdown.find("Changes since"), std::string::npos);
  EXPECT_NE(page.markdown.find("## Baseline: the current state"), std::string::npos);
  EXPECT_NE(page.markdown.find("Discord (run, user): Discord.exe --start"), std::string::npos);
  EXPECT_NE(page.markdown.find("cl.exe | 100 | 40.0 %"), std::string::npos);
}

// Reports spec, "Gap of more than a month".
TEST(ReportRenderTest, AfterAGapTheReportNamesTheMonthItComparesWith) {
  FakeService service;
  const Json before = snapshot_of(service, first_of_november - 24h * 61);
  const Json after = snapshot_of(service, first_of_november);
  const std::string text =
      render(after, diff(before, after), parse_month("2026-09"), utc()).markdown;
  EXPECT_NE(text.find("## Changes since 2026-09"), std::string::npos);
  EXPECT_NE(text.find("Compared with the snapshot of 2026-09. No snapshot exists for 2026-10, so "
                      "these changes cover more than one month."),
            std::string::npos);
}

// Reports spec, "Both formats are written" (same content) and "HTML loads nothing from the
// network".
TEST(ReportRenderTest, TheHtmlIsSelfContainedAndHoldsTheSameContent) {
  FakeService service;
  service.answer("startup.list").at("items")[0].at("name") = "<script>alert(1)</script>";
  const Json snapshot = snapshot_of(service, first_of_november);
  const Rendered page = render(snapshot, Json(nullptr), std::nullopt, utc());
  for (const std::string_view remote :
       {"http:", "https:", "//", "src=", "href=", "<link", "<script", "@import", "url("}) {
    EXPECT_EQ(page.html.find(remote), std::string::npos) << remote;
  }
  EXPECT_NE(page.html.find("default-src 'none'"), std::string::npos);
  EXPECT_NE(page.html.find("&lt;script&gt;alert(1)&lt;/script&gt;"), std::string::npos);
  EXPECT_NE(page.html.find("<svg"), std::string::npos);  // the free-space chart, inline
  for (const std::string_view line :
       {"First report, no previous snapshot to compare", "Health (30-day averages)",
        "Free space trend", "Security", "Governor", "Freezes", "Disk health",
        "Space freed by automatic cleaning", "Samsung 990 PRO", "svchost.exe"}) {
    EXPECT_NE(page.markdown.find(line), std::string::npos) << line;
    EXPECT_NE(page.html.find(line), std::string::npos) << line;
  }
}

// Repository -------------------------------------------------------------------------------------

TEST(ReportRepositoryTest, SnapshotsAndReportsAreStoredOncePerMonth) {
  const fakes::FakeClock clock(first_of_november);
  History history(clock);
  const auto september = *parse_month("2026-09");
  const auto november = *parse_month("2026-11");
  const auto save_month = [&](std::chrono::year_month month,
                              std::optional<std::chrono::year_month> against) {
    return history.store().write([&](store::Database& db) {
      return save(db, Json{{"month", month_text(month)}},
                  StoredReport{.month = month,
                               .generated_at = first_of_november,
                               .against = against,
                               .markdown_file = "r.md",
                               .html_file = "r.html",
                               .markdown = "# report",
                               .diff = against ? Json{{"from", "x"}} : Json(nullptr)});
    });
  };
  ASSERT_TRUE(save_month(september, std::nullopt).has_value());
  ASSERT_TRUE(save_month(november, september).has_value());
  const Result<void> again = save_month(november, september);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, Errc::already_exists);

  ASSERT_TRUE(history.store()
                  .read([&](store::Database& db) -> Result<void> {
                    const auto previous = newest_before(db, november).value();
                    EXPECT_TRUE(previous.has_value());
                    EXPECT_EQ(previous->first, september);
                    EXPECT_FALSE(newest_before(db, september).value().has_value());
                    const auto report = load_report(db, november).value();
                    EXPECT_TRUE(report.has_value());
                    EXPECT_EQ(report->against, september);
                    EXPECT_EQ(report->markdown, "# report");
                    EXPECT_EQ(report->html_file, std::filesystem::path("r.html"));
                    EXPECT_TRUE(load_report(db, september).value()->diff.is_null());
                    EXPECT_FALSE(load_report(db, *parse_month("2026-10")).value().has_value());
                    EXPECT_EQ(stored_months(db, true).value(),
                              (std::vector<std::chrono::year_month>{november, september}));
                    return {};
                  })
                  .has_value());
}

TEST(ReportRepositoryTest, MigrationsAreAdditive) {
  const std::vector<store::MigrationSet> sets = {report_migrations()};
  EXPECT_TRUE(store::check_additive(sets).has_value());
}

}  // namespace
}  // namespace myclean::report
