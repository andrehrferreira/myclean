#include "svc/resident_component.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ipc/alerts.hpp"
#include "core/ipc/resident.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/process_collector.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"
#include "svc/alerts_component.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 10 / 2026} + 12h;

// The process table the telemetry component would publish: a program that gains 2 MiB of
// private bytes a minute and a steady one.
class Machine {
 public:
  void at(std::int64_t minutes) {
    telemetry::ProcessView view{.time = noon + std::chrono::minutes{minutes} + 30s};
    const auto private_bytes = Bytes{(500ULL + static_cast<std::uint64_t>(minutes) * 2) << 20U};
    view.rows.push_back(telemetry::ProcessRow{
        .info = {.pid = 10, .image = "Leaky.exe", .private_bytes = private_bytes}});
    view.rows.push_back(telemetry::ProcessRow{
        .info = {.pid = 20, .image = "steady.exe", .private_bytes = Bytes{300ULL << 20U}}});
    view.trees = {telemetry::ProcessTree{.members = {0}}, telemetry::ProcessTree{.members = {1}}};
    const std::scoped_lock lock(mutex_);
    view_ = std::make_shared<const telemetry::ProcessView>(std::move(view));
  }
  [[nodiscard]] std::shared_ptr<const telemetry::ProcessView> view() const {
    const std::scoped_lock lock(mutex_);
    return view_;
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const telemetry::ProcessView> view_;
};

// The resident component reporting to the alerts component over the shipped data/alerts.toml,
// each with a store of its own.
class ResidentComponentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    machine_.at(0);
    ASSERT_TRUE(alerts_harness_.start(alerts_).has_value());
    ASSERT_TRUE(harness_.start(resident_).has_value());
  }

  // Ticks once a minute from the current minute up to and including `last`.
  void run_until(std::int64_t last) {
    for (; minute_ <= last; ++minute_) {
      machine_.at(minute_);
      clock_.set_wall(noon + std::chrono::minutes{minute_} + 30s);
      resident_.tick();
    }
  }

  [[nodiscard]] static Json ok(const testing::ComponentHarness& harness, std::string_view method,
                               const Json& params = Json::object()) {
    Result<Json> result = harness.call(method, params);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(Json());
  }

  [[nodiscard]] testing::ComponentHarness& harness() { return harness_; }
  [[nodiscard]] const testing::ComponentHarness& alerts_harness() const { return alerts_harness_; }
  [[nodiscard]] ResidentComponent& resident() { return resident_; }

 private:
  fakes::FakeClock clock_{noon};
  Machine machine_;
  rules::DiskDataFiles files_;
  testing::ComponentHarness alerts_harness_{clock_};
  AlertsComponent alerts_{files_};
  testing::ComponentHarness harness_{clock_};
  // Given no process interface at all: it can only read the table and report. No thread: the
  // test ticks.
  ResidentComponent resident_{
      ResidentSources{.view = [this] { return machine_.view(); }, .period = {}}, &alerts_};
  std::int64_t minute_ = 0;
};

TEST_F(ResidentComponentTest, SustainedGrowthRaisesTheLeakAlertWithItsTrend) {
  run_until(std::int64_t{5} * 60);
  // Five hours of growth: no alert yet.
  EXPECT_TRUE(ok(alerts_harness(), ipc::alerts_list_method).at("alerts").empty());

  run_until(std::int64_t{7} * 60);
  const Json alerts = ok(alerts_harness(), ipc::alerts_list_method).at("alerts");
  ASSERT_EQ(alerts.size(), 1U);
  EXPECT_EQ(alerts[0].at("rule"), "process.memory-leak");
  EXPECT_EQ(alerts[0].at("entity"), "leaky.exe");
  EXPECT_EQ(alerts[0].at("active"), true);
  EXPECT_GE(alerts[0].at("value").get<double>(), 6.0);
  EXPECT_NE(alerts[0].at("detail").get<std::string>().find("leaky.exe: private bytes rose"),
            std::string::npos);

  // The trend behind the alert's chart.
  const Json trend = ok(harness(), ipc::resident_trend_method, Json{{"key", "leaky.exe"}});
  EXPECT_EQ(trend.at("image"), "leaky.exe");
  EXPECT_GE(trend.at("points").size(), 80U);
  EXPECT_EQ(trend.at("points")[0].at("time"), store::to_unix_seconds(noon));
  ASSERT_FALSE(trend.at("leak").is_null());
  EXPECT_GE(trend.at("leak").at("hours").get<int>(), 6);
  EXPECT_TRUE(
      ok(harness(), ipc::resident_trend_method, Json{{"key", "steady.exe"}}).at("leak").is_null());
  const Result<Json> unknown =
      harness().call(ipc::resident_trend_method, Json{{"key", "nope.exe"}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, Errc::not_found);
}

TEST_F(ResidentComponentTest, ReportsTheThirtyDayCostOfEveryResidentProgram) {
  // 30 days of Armoury Crate every hour, and an installer that ran for two hours.
  ASSERT_TRUE(harness()
                  .store()
                  .write([](store::Database& db) -> Result<void> {
                    Result<store::Statement> insert = db.prepare(
                        "INSERT INTO resident_hour(key, time, minutes, cpu_seconds, ram_sum, "
                        "ram_peak, disk_bytes, net_bytes) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
                    if (!insert) {
                      return tl::unexpected(insert.error());
                    }
                    for (int hour = 1; hour <= 30 * 24; ++hour) {
                      const std::int64_t time =
                          store::to_unix_seconds(noon - std::chrono::hours{hour});
                      Result<void> row =
                          insert->bind(1, std::string_view{"armourycrate.service.exe"})
                              .bind(2, time)
                              .bind(3, 60)
                              .bind(4, 1.5)
                              .bind(5, std::int64_t{60} * (100 << 20))
                              .bind(6, hour == 5 ? (400 << 20) : (100 << 20))
                              .bind(7, 1000)
                              .bind(8, 10)
                              .run();
                      if (!row) {
                        return row;
                      }
                      if (hour <= 2) {
                        row = insert->bind(1, std::string_view{"setup.exe"})
                                  .bind(2, time)
                                  .bind(3, 60)
                                  .bind(4, 900.0)
                                  .bind(5, 0)
                                  .bind(6, 0)
                                  .bind(7, 0)
                                  .bind(8, 0)
                                  .run();
                        if (!row) {
                          return row;
                        }
                      }
                    }
                    return {};
                  })
                  .has_value());

  const Json report = ok(harness(), ipc::resident_costs_method);
  EXPECT_EQ(report.at("days"), 30);
  EXPECT_EQ(report.at("from"), store::to_unix_seconds(noon - std::chrono::days{30}));
  ASSERT_EQ(report.at("programs").size(), 1U);
  const Json& armoury = report.at("programs")[0];
  EXPECT_EQ(armoury.at("image"), "armourycrate.service.exe");
  EXPECT_EQ(armoury.at("key"), "armourycrate.service.exe");
  EXPECT_DOUBLE_EQ(armoury.at("hours_running").get<double>(), 720.0);
  EXPECT_DOUBLE_EQ(armoury.at("cpu_seconds").get<double>(), 1080.0);
  EXPECT_EQ(armoury.at("ram_average"), 100 << 20);
  EXPECT_EQ(armoury.at("ram_peak"), 400 << 20);
  EXPECT_EQ(armoury.at("disk_bytes"), 720'000);
  EXPECT_EQ(armoury.at("net_bytes"), 7200);
}

TEST_F(ResidentComponentTest, StopRecordsTheMinuteObservedSoFar) {
  run_until(2);
  resident().stop();
  ASSERT_TRUE(harness().store().flush().has_value());
  std::int64_t rows = 0;
  ASSERT_TRUE(harness()
                  .store()
                  .read([&](store::Database& db) -> Result<void> {
                    Result<store::Statement> count =
                        db.prepare("SELECT COUNT(*) FROM resident_hour");
                    if (!count) {
                      return tl::unexpected(count.error());
                    }
                    if (count->step().value_or(false)) {
                      rows = count->column_int(0);
                    }
                    return {};
                  })
                  .has_value());
  EXPECT_EQ(rows, 2);  // leaky.exe and steady.exe, hour 12:00
}

}  // namespace
}  // namespace myclean::svc
