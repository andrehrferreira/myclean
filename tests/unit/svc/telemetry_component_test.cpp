#include "svc/telemetry_component.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/alerts/engine.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/ipc/telemetry.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/pipeline.hpp"
#include "fake_machine.hpp"
#include "fakes/fake_clock.hpp"
#include "phase1_fakes.hpp"
#include "platform/ipc/pipe_client.hpp"
#include "svc/service.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

std::int64_t unix_seconds(WallTime time) {
  return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::vector<std::uint32_t> pids(const Json& trees) {
  std::vector<std::uint32_t> result;
  for (const Json& tree : trees) {
    result.push_back(tree.at("pid").get<std::uint32_t>());
  }
  return result;
}

// The read methods over a pipeline that the test polls by hand.
class TelemetryMethodsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<store::Store>> opened =
        store::Store::open(dir_.path() / "myclean.db", clock_);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    pipeline_ = std::make_unique<telemetry::Pipeline>(clock_, *store_);
    collectors_ = add_collectors(*pipeline_, machine_.sources);
    methods_ = std::make_unique<TelemetryMethods>(collectors_, *store_, clock_, machine_.sources);
    ASSERT_TRUE(methods_->register_methods(dispatcher_).has_value());
  }

  void poll() {
    for (const Error& error : pipeline_->poll()) {
      ADD_FAILURE() << error.message;
    }
  }

  // Time passes with the browser busy, then the pipeline samples.
  void advance(std::chrono::seconds elapsed) {
    testing::burn_browser_cpu(*machine_.processes, elapsed);
    clock_.advance(elapsed);
    poll();
  }

  // The raw JSON-RPC response to a request.
  [[nodiscard]] Json raw(std::string_view method, const Json& params) const {
    return Json::parse(dispatcher_.handle(ipc::make_request(7, method, params)));
  }

  [[nodiscard]] Result<Json> call(std::string_view method,
                                  const Json& params = Json::object()) const {
    return ipc::parse_response(dispatcher_.handle(ipc::make_request(7, method, params)), 7);
  }

  [[nodiscard]] Json ok(std::string_view method, const Json& params = Json::object()) const {
    Result<Json> result = call(method, params);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return result.value_or(Json());
  }

  [[nodiscard]] fakes::FakeClock& clock() { return clock_; }
  [[nodiscard]] testing::FakeMachine& machine() { return machine_; }
  [[nodiscard]] const Collectors& collectors() const { return collectors_; }
  [[nodiscard]] const TelemetryMethods& methods() const { return *methods_; }
  [[nodiscard]] store::Store& store() { return *store_; }

 private:
  testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  testing::FakeMachine machine_ = testing::fake_machine();
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<telemetry::Pipeline> pipeline_;
  Collectors collectors_;
  std::unique_ptr<TelemetryMethods> methods_;
  ipc::Dispatcher dispatcher_;
};

TEST_F(TelemetryMethodsTest, LatestGroupsTheNewestMeasurementsOfEveryCollector) {
  poll();
  const Json latest = ok(ipc::metrics_latest_method);

  EXPECT_EQ(latest.at("time"), unix_seconds(noon));
  EXPECT_EQ(latest.at("cpu").at("utility"), 12.5);
  EXPECT_EQ(latest.at("cpu").at("dpc"), 0.25);
  EXPECT_EQ(latest.at("cpu").at("logical_processors"), 32);
  EXPECT_EQ(latest.at("memory").at("total"), static_cast<double>((64_GiB).count()));
  EXPECT_EQ(latest.at("memory").at("commit_limit"), static_cast<double>((80_GiB).count()));
  EXPECT_EQ(latest.at("process").at("count"), 5.0);

  const Json& disks = latest.at("disks");
  ASSERT_EQ(disks.size(), 2U);
  EXPECT_EQ(disks[0].at("disk"), "2");  // disk numbers in numeric order
  EXPECT_EQ(disks[0].at("queue"), 2.0);
  EXPECT_EQ(disks[0].at("read_bytes"), 4096.0);
  EXPECT_EQ(disks[1].at("disk"), "10");
  EXPECT_FALSE(disks[1].contains("read_bytes"));  // not measured: left out

  EXPECT_TRUE(latest.at("gpu").is_null());
  ASSERT_EQ(latest.at("unavailable").size(), 1U);
  EXPECT_TRUE(latest.at("unavailable")[0].get<std::string>().starts_with("gpu: "));

  const Json& volumes = latest.at("volumes");
  ASSERT_EQ(volumes.size(), 1U);
  EXPECT_EQ(volumes[0].at("volume"), "C:");
  EXPECT_EQ(volumes[0].at("entity"), "C");
  EXPECT_EQ(volumes[0].at("system"), true);
  EXPECT_EQ(volumes[0].at("free_percent"), 10.0);
  EXPECT_EQ(volumes[0].at("disks"), Json::array({2}));
  EXPECT_EQ(volumes[0].at("fill").at("state"), "insufficient_data");
  EXPECT_TRUE(volumes[0].at("fill").at("days_until_full").is_null());
}

TEST_F(TelemetryMethodsTest, LatestListsLogicalProcessorsInNaturalOrder) {
  machine().counters->set(R"(\Processor Information(*)\% Processor Utility)",
                          {{.instance = "0,10", .value = 10.0},
                           {.instance = "1,0", .value = 20.0},
                           {.instance = "0,2", .value = 30.0},
                           {.instance = "0,_Total", .value = 50.0},
                           {.instance = "_Total", .value = 12.5}});
  machine().counters->set(R"(\Processor Information(*)\% of Maximum Frequency)",
                          {{.instance = "0,2", .value = 95.0}});
  poll();
  const Json cpu = ok(ipc::metrics_latest_method).at("cpu");
  EXPECT_EQ(cpu.at("utility"), 12.5);
  EXPECT_EQ(cpu.at("processors"),
            (Json::array({{{"entity", "0_2"}, {"utility", 30.0}, {"frequency", 95.0}},
                          {{"entity", "0_10"}, {"utility", 10.0}},
                          {{"entity", "1_0"}, {"utility", 20.0}}})));
}

TEST_F(TelemetryMethodsTest, ExplainDescribesAProcessAndItsTree) {
  poll();
  advance(1s);
  const Json child = ok(ipc::processes_explain_method, Json{{"pid", 101}});
  EXPECT_EQ(child.at("image"), "browser.exe");
  EXPECT_EQ(child.at("parent_pid"), 100);
  EXPECT_EQ(child.at("private_working_set"), (200_MiB).count());
  EXPECT_EQ(child.at("tree").at("pid"), 100);
  EXPECT_EQ(child.at("tree").at("processes"), 2);
  EXPECT_NEAR(child.at("tree").at("cpu_percent").get<double>(), 9.0, 1e-9);
  EXPECT_EQ(pids(child.at("members")), (std::vector<std::uint32_t>{100, 101}));
  EXPECT_TRUE(child.at("trust").is_null());  // no trust signal wired here
  EXPECT_TRUE(child.at("kb").is_null());
  EXPECT_TRUE(child.at("path").is_null());

  const Result<Json> gone = call(ipc::processes_explain_method, Json{{"pid", 4242}});
  ASSERT_FALSE(gone.has_value());
  EXPECT_EQ(gone.error().code, Errc::not_found);
  for (const std::int64_t pid : {std::int64_t{-1}, std::int64_t{1} << 32}) {
    const Result<Json> refused = call(ipc::processes_explain_method, Json{{"pid", pid}});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, Errc::invalid_argument);
  }
  EXPECT_EQ(raw(ipc::processes_explain_method, Json::object()).at("error").at("code"),
            ipc::rpc_error::invalid_params);
}

TEST_F(TelemetryMethodsTest, VolumeFactsCarryFreeSpaceAndDaysUntilFull) {
  poll();
  Result<std::vector<alerts::Reading>> facts = methods().volume_facts();
  ASSERT_TRUE(facts.has_value()) << facts.error().message;
  ASSERT_EQ(facts->size(), 2U);  // too little history for a fill projection
  EXPECT_EQ(facts->at(0).metric, "volume.free_percent");  // C: is the system volume
  EXPECT_EQ(facts->at(0).entity, "C:");
  EXPECT_EQ(facts->at(0).value, 10.0);
  EXPECT_EQ(facts->at(0).detail, "C: 100.0 GiB free of 1000.0 GiB");
  EXPECT_EQ(facts->at(1).metric, "volume.free_gib");
  EXPECT_EQ(facts->at(1).value, 100.0);

  // Three hours without change: not filling, so days until full is infinite.
  for (int minute = 1; minute <= 180; ++minute) {
    advance(60s);
  }
  facts = methods().volume_facts();
  ASSERT_TRUE(facts.has_value()) << facts.error().message;
  ASSERT_EQ(facts->size(), 3U);
  EXPECT_EQ(facts->at(2).metric, "volume.days_until_full");
  EXPECT_TRUE(std::isinf(facts->at(2).value));
}

TEST_F(TelemetryMethodsTest, TopListsTheQualifyingTreesButNeverTheIdleProcess) {
  poll();
  advance(1s);

  const Json qualifying = ok(ipc::processes_top_method, Json{{"qualifying", true}});
  EXPECT_EQ(pids(qualifying.at("trees")), (std::vector<std::uint32_t>{100, 200}));
  EXPECT_EQ(qualifying.at("filter"), (Json{{"cpu_percent", 5.0}, {"memory_percent", 5.0}}));
  EXPECT_EQ(qualifying.at("total_trees"), 3);  // browser, editor, small; never the idle process
  EXPECT_EQ(qualifying.at("logical_processors"), 32);
  EXPECT_EQ(qualifying.at("memory_total"), (64_GiB).count());

  const Json& browser = qualifying.at("trees")[0];
  EXPECT_EQ(browser.at("image"), "browser.exe");
  EXPECT_EQ(browser.at("processes"), 2);
  EXPECT_NEAR(browser.at("cpu_percent").get<double>(), 9.0, 1e-9);
  EXPECT_EQ(browser.at("private_working_set"), (500_MiB).count());
  EXPECT_EQ(pids(browser.at("members")), (std::vector<std::uint32_t>{100, 101}));
  EXPECT_TRUE(browser.at("trust").is_null());
  const Json& editor = qualifying.at("trees")[1];
  EXPECT_DOUBLE_EQ(editor.at("memory_percent").get<double>(), 12.5);

  const Json by_memory = ok(ipc::processes_top_method, Json{{"sort", "memory"}, {"limit", 3}});
  EXPECT_EQ(pids(by_memory.at("trees")), (std::vector<std::uint32_t>{200, 100, 300}));
  EXPECT_TRUE(by_memory.at("filter").is_null());

  for (const Json& params : {Json{{"limit", 0}}, Json{{"limit", 101}}, Json{{"sort", "disk"}}}) {
    const Result<Json> refused = call(ipc::processes_top_method, params);
    ASSERT_FALSE(refused.has_value()) << params.dump();
    EXPECT_EQ(refused.error().code, Errc::invalid_argument);
  }
}

TEST_F(TelemetryMethodsTest, QueryResolvesEntityTokensAndMergesPoints) {
  poll();
  for (int second = 1; second <= 9; ++second) {  // 10 %, 20 %, ... 90 %
    machine().counters->set(R"(\Processor Information(*)\% Processor Utility)",
                            {{.instance = "_Total", .value = 10.0 * second}});
    advance(1s);
  }
  const std::int64_t now = unix_seconds(clock().now());

  const Json all =
      ok(ipc::metrics_query_method, Json{{"metric", "cpu.utility"}, {"from", now - 60}});
  EXPECT_EQ(all.at("resolution"), "raw");
  EXPECT_EQ(all.at("entity"), "system");
  ASSERT_EQ(all.at("points").size(), 10U);  // 12.5 % at the first poll, then 10 % to 90 %

  // Buckets of 4: {12.5, 10, 20, 30}, {40, 50, 60, 70}, {80, 90}.
  const Json merged = ok(ipc::metrics_query_method,
                         Json{{"metric", "cpu.utility"}, {"from", now - 60}, {"max_points", 3}});
  const Json& points = merged.at("points");
  ASSERT_EQ(points.size(), 3U);
  EXPECT_EQ(points[0].at("count"), 4);
  EXPECT_EQ(points[0].at("min"), 10.0);
  EXPECT_EQ(points[0].at("max"), 30.0);
  EXPECT_DOUBLE_EQ(points[0].at("mean").get<double>(), 18.125);
  EXPECT_EQ(points[0].at("time"), unix_seconds(noon));
  EXPECT_EQ(points[1].at("time"), all.at("points")[4].at("time"));
  EXPECT_EQ(points[2].at("count"), 2);
  EXPECT_DOUBLE_EQ(points[2].at("mean").get<double>(), 85.0);

  const Json before_now = ok(ipc::metrics_query_method,
                             Json{{"metric", "cpu.utility"}, {"from", now - 60}, {"to", now}});
  EXPECT_EQ(before_now.at("points").size(), 9U);  // `to` is exclusive

  const Json volume = ok(ipc::metrics_query_method,
                         Json{{"metric", "volume.free"}, {"entity", "C"}, {"from", now - 60}});
  EXPECT_EQ(volume.at("entity"), "C:");
  ASSERT_EQ(volume.at("points").size(), 1U);
  EXPECT_EQ(volume.at("points")[0].at("mean"), static_cast<double>((100_GiB).count()));

  const Result<Json> unknown =
      call(ipc::metrics_query_method, Json{{"metric", "cpu.bogus"}, {"from", now - 60}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, Errc::invalid_argument);
  EXPECT_NE(unknown.error().message.find("known metrics: "), std::string::npos);
  EXPECT_NE(unknown.error().message.find("cpu.utility"), std::string::npos);

  for (const Json& params : {Json{{"metric", "cpu.utility"}, {"from", now}, {"to", now}},
                             Json{{"metric", "cpu.utility"}, {"from", -1}},
                             Json{{"metric", "cpu.utility"}, {"from", 0}, {"max_points", 0}}}) {
    const Result<Json> refused = call(ipc::metrics_query_method, params);
    ASSERT_FALSE(refused.has_value()) << params.dump();
    EXPECT_EQ(refused.error().code, Errc::invalid_argument);
  }
}

TEST_F(TelemetryMethodsTest, RefusesPathsAndUndeclaredParameters) {
  poll();
  for (const Json& params : {Json{{"metric", "volume.free"}, {"entity", "C:\\"}, {"from", 0}},
                             Json{{"metric", "cpu.utility"}, {"from", 0}, {"path", "C"}},
                             Json{{"metric", "cpu.utility"}, {"from", "0"}}}) {
    EXPECT_EQ(raw(ipc::metrics_query_method, params).at("error").at("code"),
              ipc::rpc_error::invalid_params)
        << params.dump();
  }
  EXPECT_EQ(raw(ipc::processes_top_method, Json{{"command", "taskkill"}}).at("error").at("code"),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(raw(ipc::volumes_list_method, Json{{"volume", "C"}}).at("error").at("code"),
            ipc::rpc_error::invalid_params);
  EXPECT_EQ(raw("processes.kill", Json{{"pid", 100}}).at("error").at("code"),
            ipc::rpc_error::method_not_found);
}

TEST_F(TelemetryMethodsTest, VolumesProjectDaysUntilFull) {
  // C: loses 1 GiB an hour for three hours, sampled every minute.
  poll();
  telemetry::VolumeStatus& volume = machine().volumes->list().front();
  for (int minute = 1; minute <= 180; ++minute) {
    volume.free =
        Bytes{(100_GiB).count() - (static_cast<std::uint64_t>(minute) * (1_GiB).count() / 60)};
    advance(60s);
  }
  const Json volumes = ok(ipc::volumes_list_method);
  EXPECT_EQ(volumes.at("time"), unix_seconds(clock().now()));
  const Json& fill = volumes.at("volumes").at(0).at("fill");
  EXPECT_EQ(fill.at("state"), "filling");
  const double gib_per_day = fill.at("bytes_per_day").get<double>() / (1_GiB).count();
  EXPECT_NEAR(gib_per_day, -24.0, 0.01);
  // About 97 GiB left at 24 GiB a day.
  EXPECT_NEAR(fill.at("days_until_full").get<double>(), 97.0 / 24.0, 0.02);

  const Result<std::vector<alerts::Reading>> facts = methods().volume_facts();
  ASSERT_TRUE(facts.has_value()) << facts.error().message;
  ASSERT_EQ(facts->size(), 3U);
  EXPECT_EQ(facts->at(2).metric, "volume.days_until_full");
  EXPECT_NEAR(facts->at(2).value, 97.0 / 24.0, 0.02);
}

TEST_F(TelemetryMethodsTest, TheFillProjectionIsComputedOncePerVolumeSample) {
  // C: loses 1 GiB an hour for three hours, sampled every minute.
  poll();
  telemetry::VolumeStatus& volume = machine().volumes->list().front();
  for (int minute = 1; minute <= 180; ++minute) {
    volume.free =
        Bytes{(100_GiB).count() - (static_cast<std::uint64_t>(minute) * (1_GiB).count() / 60)};
    advance(60s);
  }
  const Json filling = ok(ipc::metrics_latest_method).at("volumes").at(0).at("fill");
  ASSERT_EQ(filling.at("state"), "filling");

  // Six days ago C: had far less free space: a fresh projection would say "not filling".
  const std::int64_t six_days_ago = unix_seconds(clock().now() - std::chrono::days{6});
  const std::string insert = std::format(
      "INSERT INTO ts_minute(series, time, min, max, mean, count) SELECT id, {}, 1, 1, 1, 60 "
      "FROM series WHERE metric = 'volume.free' AND entity = 'C:'",
      six_days_ago);
  ASSERT_TRUE(
      store().write([&](store::Database& database) { return database.exec(insert); }).has_value());

  // The 1 s collectors sample again, but not the volumes: the projection of this sample stays.
  advance(1s);
  EXPECT_EQ(ok(ipc::metrics_latest_method).at("volumes").at(0).at("fill"), filling);
  EXPECT_EQ(ok(ipc::volumes_list_method).at("volumes").at(0).at("fill"), filling);

  // The next volume sample computes it again.
  advance(59s);
  EXPECT_EQ(ok(ipc::metrics_latest_method).at("volumes").at(0).at("fill").at("state"),
            "not_filling");
}

// The component inside a running service, over the pipe.
class TelemetryComponentTest : public ::testing::Test {
 protected:
  [[nodiscard]] Result<std::unique_ptr<Service>> start(testing::FakeMachine machine) {
    return testing::start_telemetry_service(std::move(machine), dir_.path(), pipe_, clock_);
  }

  [[nodiscard]] platform::PipeClient connect() const {
    Result<platform::PipeClient> client = platform::PipeClient::connect(pipe_, 5s);
    EXPECT_TRUE(client.has_value()) << client.error().message;
    return std::move(*client);
  }

 private:
  testing::TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::wstring pipe_ = testing::unique_pipe_name();
};

TEST_F(TelemetryComponentTest, ServesTheReadMethodsAndReportsItsHealth) {
  Result<std::unique_ptr<Service>> service = start(testing::fake_machine());
  ASSERT_TRUE(service.has_value()) << service.error().message;
  platform::PipeClient client = connect();

  const Result<ipc::StatusReply> status = client.status();
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_TRUE(status->healthy);
  EXPECT_EQ(status->components, std::vector<std::string>{"telemetry"});
  ASSERT_EQ(status->component_health.size(), 1U);
  const ipc::ComponentHealth& health = status->component_health.front();
  EXPECT_EQ(health.name, "telemetry");
  EXPECT_TRUE(health.healthy);
  EXPECT_TRUE(health.problems.empty());
  ASSERT_EQ(health.unavailable.size(), 1U);
  EXPECT_TRUE(health.unavailable.front().starts_with("gpu: "));

  // The first sample was taken during start: the methods answer at once.
  const Result<Json> top = client.call(ipc::processes_top_method, Json{{"qualifying", true}});
  ASSERT_TRUE(top.has_value()) << top.error().message;
  EXPECT_EQ(pids(top->at("trees")), std::vector<std::uint32_t>{200});
  const Result<Json> latest = client.call(ipc::metrics_latest_method);
  ASSERT_TRUE(latest.has_value()) << latest.error().message;
  EXPECT_EQ(latest->at("memory").at("total"), static_cast<double>((64_GiB).count()));

  const auto stopping = std::chrono::steady_clock::now();
  EXPECT_TRUE((*service)->stop().has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - stopping, 5s);
}

TEST(TelemetryComponent, ReportsVolumeFactsToTheAlertSinkOncePerVolumeSample) {
  const testing::TempDir dir;
  const fakes::FakeClock clock{noon};
  testing::RecordingAlertSink sink;
  const std::vector<store::MigrationSet> sets;
  Result<std::unique_ptr<store::Store>> store = store::Store::open(dir.path() / "db", clock, sets);
  ASSERT_TRUE(store.has_value()) << store.error().message;
  ipc::Dispatcher methods;
  TelemetryComponent telemetry{testing::fake_machine().sources, {}, &sink};
  ASSERT_TRUE(telemetry.start(Context(**store, methods, clock, dir.path())).has_value());
  telemetry.stop();

  // The first sample, taken in start(): the free space of C:, the system volume, as a share and
  // in GiB.
  const std::vector<alerts::Reading> facts = sink.facts();
  ASSERT_EQ(facts.size(), 2U);
  EXPECT_EQ(facts[0].metric, "volume.free_percent");
  EXPECT_EQ(facts[0].entity, "C:");
  EXPECT_EQ(facts[1].metric, "volume.free_gib");
  EXPECT_EQ(facts[1].entity, "C:");
  ASSERT_NE(telemetry.volumes(), nullptr);
  EXPECT_EQ(telemetry.volumes()->volumes.size(), 1U);
}

TEST_F(TelemetryComponentTest, AFailingCollectorMakesTheServiceDegraded) {
  testing::FakeMachine machine = testing::fake_machine();
  machine.volumes->fail_with("the device is not ready");
  Result<std::unique_ptr<Service>> service = start(std::move(machine));
  ASSERT_TRUE(service.has_value()) << service.error().message;

  const Result<ipc::StatusReply> status = connect().status();
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_FALSE(status->healthy);
  EXPECT_FALSE(status->store.error.has_value());
  ASSERT_EQ(status->component_health.size(), 1U);
  EXPECT_FALSE(status->component_health.front().healthy);
  EXPECT_EQ(status->component_health.front().problems,
            std::vector<std::string>{"volumes: the device is not ready"});
}

}  // namespace
}  // namespace myclean::svc
