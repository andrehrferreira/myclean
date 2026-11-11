#include "svc/report_component.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/actions/schema.hpp"
#include "core/alerts/engine.hpp"
#include "core/governing/records.hpp"
#include "core/health/analysis.hpp"
#include "core/input/incident_store.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/reports.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/report/repository.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "report/report_fakes.hpp"
#include "svc/component.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using report::testing::FakeService;

WallTime at(std::chrono::year_month_day day, std::chrono::minutes time) {
  return std::chrono::sys_days{day} + time;
}

class Alerts final : public health::IAlertSink {
 public:
  void report(std::span<const alerts::Reading> readings) override {
    seen.insert(seen.end(), readings.begin(), readings.end());
  }
  std::vector<alerts::Reading> seen;
};

std::string read_file(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The service as the report component sees it: one store with the tables it reads, kept across
// restarts of the component, and a fresh method table per start.
class ReportComponentTest : public ::testing::Test {
 protected:
  ReportComponentTest() {
    const std::vector<store::MigrationSet> sets = {
        report::report_migrations(), actions::action_migrations(), governing::governor_migrations(),
        input::input_migrations()};
    store_ = store::Store::open(dir_.path() / "myclean.db", clock_, sets).value();
  }

  // Starts a new component, as a service start does, without a thread.
  void start() {
    component_.reset();
    methods_ = std::make_unique<ipc::Dispatcher>();
    component_ = std::make_unique<ReportComponent>(ReportSources{.call = service_.caller(),
                                                                 .reports_dir = {},
                                                                 .warm_up = 0s,
                                                                 .poll_period = 0s,
                                                                 .analysis_poll = 0s,
                                                                 .max_polls = 3},
                                                   &alerts_);
    ASSERT_TRUE(
        component_
            ->start(Context(*store_, *methods_, clock_, dir_.path() / "data", {}, dir_.path()))
            .has_value());
  }

  void poll() { component_->poll_now(std::stop_token{}); }

  [[nodiscard]] Json call(std::string_view method, const Json& params) const {
    Result<Json> result =
        ipc::parse_response(methods_->handle(ipc::make_request(3, method, params)), 3);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return result.value_or(Json());
  }
  [[nodiscard]] Json report_of(std::string_view month) const {
    return call(ipc::reports_get_method, Json{{"month", month}});
  }
  [[nodiscard]] std::int64_t count(std::string_view sql) const {
    return testing::query_int(dir_.path() / "myclean.db", sql);
  }

  // Stops the running component, as a service stop does.
  void stop_component() { component_.reset(); }

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_.path(); }
  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] FakeService& service() { return service_; }
  [[nodiscard]] const std::vector<alerts::Reading>& seen() const { return alerts_.seen; }
  [[nodiscard]] ReportComponent& component() { return *component_; }
  [[nodiscard]] const ipc::Dispatcher& methods() const { return *methods_; }

 private:
  testing::TempDir dir_;
  fakes::FakeClock clock_{at(std::chrono::November / 1 / 2026, 8h + 59min)};
  std::unique_ptr<store::Store> store_;
  FakeService service_;
  Alerts alerts_;
  std::unique_ptr<ipc::Dispatcher> methods_;
  std::unique_ptr<ReportComponent> component_;
};

// Reports spec, "Runs on day 1 at 09:00": nothing at 08:59, the report of 2026-11 at 09:00, with
// both files and the toast reading; "First-ever report".
TEST_F(ReportComponentTest, RunsOnTheFirstAtNineAndWritesBothFiles) {
  start();
  poll();
  EXPECT_FALSE(report_of("2026-11").at("found"));
  clock().advance(1min);
  poll();

  const Json answer = report_of("2026-11");
  ASSERT_TRUE(answer.at("found")) << answer.dump();
  EXPECT_TRUE(answer.at("first_report"));
  EXPECT_TRUE(answer.at("against").is_null());
  EXPECT_TRUE(answer.at("diff").is_null());
  const std::filesystem::path html = dir() / "reports" / "2026-11" / "report.html";
  const std::filesystem::path markdown = dir() / "reports" / "2026-11" / "report.md";
  const std::u8string html_text = html.u8string();
  EXPECT_EQ(answer.at("files").at("html"), std::string(html_text.begin(), html_text.end()));
  ASSERT_TRUE(std::filesystem::exists(html));
  ASSERT_TRUE(std::filesystem::exists(markdown));
  EXPECT_EQ(read_file(markdown), answer.at("markdown").get<std::string>());
  EXPECT_NE(read_file(markdown).find("First report, no previous snapshot to compare"),
            std::string::npos);
  EXPECT_NE(read_file(html).find("First report, no previous snapshot to compare"),
            std::string::npos);
  EXPECT_EQ(read_file(html).find("http:"), std::string::npos);  // no remote resource

  ASSERT_EQ(seen().size(), 1U);
  EXPECT_EQ(seen()[0].metric, "report.ready");
  EXPECT_EQ(seen()[0].entity, "2026-11");
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run WHERE job = 'monthly-report' AND "
                  "status = 'succeeded' AND catch_up = 0"),
            1);
}

// Reports spec, "Catch-up at next boot" and "No duplicate run in the same month".
TEST_F(ReportComponentTest, AMissedMonthRunsOnceAtTheNextStart) {
  clock().set_wall(at(std::chrono::November / 30 / 2026, 12h));
  start();
  poll();
  stop_component();  // the PC is off from 2026-11-30 until 2026-12-03 10:00

  clock().set_wall(at(std::chrono::December / 3 / 2026, 10h));
  start();
  poll();
  EXPECT_TRUE(report_of("2026-12").at("found"));
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run WHERE catch_up = 1 AND status = 'succeeded'"),
            1);

  clock().set_wall(at(std::chrono::December / 4 / 2026, 10h));
  start();
  poll();
  poll();
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run"), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM snapshot"), 1);
  EXPECT_EQ(seen().size(), 1U);
}

// A run of this month that MyClean stopped during runs again at the next start, then never.
TEST_F(ReportComponentTest, AnInterruptedRunOfThisMonthRunsAgainAtTheNextStart) {
  service().keep_scanning(1000);
  start();
  clock().advance(1min);
  std::stop_source stopping;
  stopping.request_stop();
  component().poll_now(stopping.get_token());
  EXPECT_FALSE(report_of("2026-11").at("found"));
  EXPECT_EQ(count("SELECT COUNT(*) FROM schedule_run WHERE status = 'failed'"), 1);

  service().keep_scanning(0);
  clock().advance(1h);
  start();
  poll();
  EXPECT_TRUE(report_of("2026-11").at("found"));

  start();
  poll();
  EXPECT_EQ(count("SELECT COUNT(*) FROM snapshot"), 1);
  EXPECT_EQ(seen().size(), 1U);
}

// Reports spec, "Gap of more than a month" and "New extension and listening port appear in the
// diff", through the stored report and compare_snapshots.
TEST_F(ReportComponentTest, TheNextReportComparesWithTheNewestSnapshotAndNamesItsMonth) {
  clock().set_wall(at(std::chrono::September / 1 / 2026, 8h + 59min));
  start();
  clock().advance(1min);
  poll();
  ASSERT_TRUE(report_of("2026-09").at("found"));

  service()
      .answer("security.extensions")
      .at("extensions")
      .push_back(FakeService::extension("abc", "Coupon Helper", nullptr));
  service()
      .answer("security.network")
      .at("processes")
      .push_back(Json{{"pid", 4242},
                      {"image", "node.exe"},
                      {"listening", Json::array({{{"protocol", "tcp"}, {"port", 8080}}})}});
  clock().set_wall(at(std::chrono::November / 1 / 2026, 9h + 1min));  // October was missed
  start();
  poll();

  const Json answer = report_of("2026-11");
  ASSERT_TRUE(answer.at("found"));
  EXPECT_EQ(answer.at("against"), "2026-09");
  EXPECT_FALSE(answer.at("first_report"));
  const Json sets = answer.at("diff").at("sets");
  EXPECT_EQ(sets.at("browser_extensions").at("added")[0].at("key"), "chrome/Default/abc");
  EXPECT_EQ(sets.at("listening_ports").at("added")[0].at("key"), "tcp/8080/node.exe");
  const std::string text = answer.at("markdown").get<std::string>();
  EXPECT_NE(text.find("Compared with the snapshot of 2026-09. No snapshot exists for 2026-10"),
            std::string::npos);
  EXPECT_NE(text.find("New: TCP 8080 by node.exe (pid 4242)"), std::string::npos);

  const Json listed = call(ipc::reports_list_method, Json::object()).at("reports");
  ASSERT_EQ(listed.size(), 2U);
  EXPECT_EQ(listed[0].at("month"), "2026-11");
  EXPECT_EQ(listed[0].at("snapshot"), "2026-11");
  EXPECT_EQ(listed[0].at("against"), "2026-09");
  EXPECT_EQ(listed[0].at("files"), answer.at("files"));
  EXPECT_EQ(listed[1].at("month"), "2026-09");
  EXPECT_TRUE(listed[1].at("first_report"));

  const Json compared = call(ipc::reports_compare_method, Json{{"a", "2026-09"}, {"b", "2026-11"}});
  ASSERT_TRUE(compared.at("found"));
  EXPECT_EQ(compared.at("diff"), answer.at("diff"));
  EXPECT_NE(compared.at("markdown").get<std::string>().find("Changes from 2026-09 to 2026-11"),
            std::string::npos);
}

// Reports spec, "Missing month": a clear "not found", never an empty report.
TEST_F(ReportComponentTest, AMissingMonthOrSnapshotIsNotFound) {
  start();
  EXPECT_EQ(call(ipc::reports_list_method, Json::object()), (Json{{"reports", Json::array()}}));
  clock().advance(1min);
  poll();
  const Json missing = report_of("2026-05");
  EXPECT_EQ(missing, (Json{{"found", false},
                           {"month", "2026-05"},
                           {"message", "no report exists for 2026-05"},
                           {"available", Json::array({"2026-11"})}}));
  const Json compared = call(ipc::reports_compare_method, Json{{"a", "2026-05"}, {"b", "2026-11"}});
  EXPECT_EQ(compared.at("found"), false);
  EXPECT_EQ(compared.at("missing"), Json::array({"2026-05"}));
  EXPECT_EQ(compared.at("message"), "no snapshot exists for 2026-05");

  const Result<Json> bad = ipc::parse_response(
      methods().handle(ipc::make_request(4, ipc::reports_get_method, Json{{"month", "May"}})), 4);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, Errc::invalid_argument);
}

// Reports spec, "Audit log shows no action from the report run": the job calls read methods only
// and writes nothing to action_log.
TEST_F(ReportComponentTest, TheMonthlyJobOnlyReads) {
  start();
  clock().advance(1min);
  poll();
  ASSERT_TRUE(report_of("2026-11").at("found"));
  EXPECT_EQ(count("SELECT COUNT(*) FROM action_log"), 0);
  const std::set<std::string, std::less<>> reads = {
      "volumes.list",          "space.top",     "space.old_downloads", "space.duplicates",
      "space.used_installers", "startup.list",  "security.extensions", "security.network",
      "security.scan",         "processes.top", "disks.health",        "metrics.query",
      "metrics.latest",        "resident.costs"};
  for (const auto& [method, params] : service().calls()) {
    EXPECT_TRUE(reads.contains(method)) << method;
  }
}

}  // namespace
}  // namespace myclean::svc
