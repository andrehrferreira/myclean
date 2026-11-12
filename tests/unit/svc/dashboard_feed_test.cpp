// NFR-PERF-2, "live charts at 1 Hz without dropping samples", end to end on a fake clock: the
// service's telemetry (its pipeline and read methods over a fake machine) feeds the tray's poller
// and dashboard model, in-process, for ten minutes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/ipc/json_rpc.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/pipeline.hpp"
#include "core/tray_model/chart.hpp"
#include "core/tray_model/dashboard_model.hpp"
#include "core/tray_model/poller.hpp"
#include "core/tray_model/service_link.hpp"
#include "fake_machine.hpp"
#include "fakes/fake_clock.hpp"
#include "svc/telemetry_component.hpp"
#include "svc_test_support.hpp"

namespace myclean::svc {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::November / 11 / 2026} + 12h;

// The tray's link to the service without the pipe: each call goes to the service's dispatcher.
class DispatcherLink final : public tray_model::IServiceLink {
 public:
  explicit DispatcherLink(const ipc::Dispatcher& methods) : methods_(&methods) {}

  [[nodiscard]] Result<Json> call(std::string_view method, const Json& params) override {
    ++id_;
    return ipc::parse_response(methods_->handle(ipc::make_request(id_, method, params)), id_);
  }
  [[nodiscard]] bool reachable() const override { return true; }

 private:
  const ipc::Dispatcher* methods_;
  std::int64_t id_ = 0;
};

// The series the store holds under the key the tray charts: metrics.latest gives a logical
// processor's entity in token form ("0_1" for "0,1").
SeriesKey store_key(SeriesKey key) {
  if (key.metric == "cpu.utility" && key.entity != "system") {
    std::ranges::replace(key.entity, '_', ',');
  }
  return key;
}

TEST(DashboardFeedTest, TenMinutesOfLiveChartsHoldEverySampleTheStoreHolds) {
  const testing::TempDir dir;
  fakes::FakeClock clock{noon};
  testing::FakeMachine machine = testing::fake_machine();
  Result<std::unique_ptr<store::Store>> opened =
      store::Store::open(dir.path() / "myclean.db", clock);
  ASSERT_TRUE(opened.has_value()) << opened.error().message;
  store::Store& store = **opened;
  telemetry::Pipeline pipeline(clock, store);
  const Collectors collectors = add_collectors(pipeline, machine.sources);
  const TelemetryMethods methods(collectors, store, clock, machine.sources);
  ipc::Dispatcher dispatcher;
  ASSERT_TRUE(methods.register_methods(dispatcher).has_value());
  DispatcherLink link(dispatcher);
  tray_model::Poller poller(link, tray_model::live_window);
  tray_model::DashboardModel model;

  std::vector<SeriesKey> charted;
  for (int second = 0; second <= 600; ++second) {
    // The machine changes every second, so that each sample differs from the one before.
    machine.counters->set(R"(\Processor Information(*)\% Processor Utility)",
                          {{.instance = "0,0", .value = 10.0 + (second % 37)},
                           {.instance = "_Total", .value = 5.0 + (second % 53)}});
    testing::burn_browser_cpu(*machine.processes, 1s);
    if (second > 0) {
      clock.advance(1s);
    }
    for (const Error& error : pipeline.poll()) {
      ADD_FAILURE() << error.message;
    }
    // The tray's poll thread is late now and then (a busy machine): it skips up to 4 seconds.
    if (second % 97 >= 1 && second % 97 <= 4) {
      continue;
    }
    tray_model::PollResult result = poller.poll(clock.now(), true);
    ASSERT_TRUE(result.reachable) << result.error;
    if (!result.latest) {
      FAIL() << "no metrics.latest at second " << second;
    }
    charted.clear();
    for (const auto& [key, sample] : result.latest->metrics.values()) {
      charted.push_back(key);
    }
    model.apply(std::move(result));
  }

  ASSERT_FALSE(charted.empty());
  const double newest = tray_model::chart_time(clock.now());
  const double window = std::chrono::duration<double>(tray_model::live_window).count();
  int compared = 0;
  for (const SeriesKey& key : charted) {
    const tray_model::ChartSeries* chart = model.chart(key);
    ASSERT_NE(chart, nullptr) << key.metric << " " << key.entity;
    std::vector<double> times;
    std::vector<double> values;
    for (const Sample& sample : store.raw(store_key(key))) {
      if (tray_model::chart_time(sample.time) >= newest - window) {
        times.push_back(tray_model::chart_time(sample.time));
        values.push_back(sample.value);
      }
    }
    ASSERT_FALSE(times.empty()) << key.metric << " " << key.entity;
    EXPECT_EQ(std::vector<double>(chart->times().begin(), chart->times().end()), times)
        << key.metric << " " << key.entity;
    EXPECT_EQ(std::vector<double>(chart->values().begin(), chart->values().end()), values)
        << key.metric << " " << key.entity;
    ++compared;
  }
  // The 1 s series (CPU, the processors, memory, the disks) span the whole window.
  const tray_model::ChartSeries* cpu =
      model.chart(SeriesKey{.metric = "cpu.utility", .entity = "system"});
  ASSERT_NE(cpu, nullptr);
  EXPECT_GE(cpu->times().size(), 600U);
  EXPECT_GT(compared, 3);
}

}  // namespace
}  // namespace myclean::svc
