#include "core/telemetry/gpu_collector.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "fake_sources.hpp"

namespace myclean::telemetry {
namespace {

using namespace myclean::literals;

constexpr std::string_view engines = R"(\GPU Engine(*)\Utilization Percentage)";
constexpr std::string_view adapter = "luid_0x00000000_0x0000D1B5_phys_0";

std::string engine(int pid, int index, std::string_view type) {
  return "pid_" + std::to_string(pid) + "_" + std::string(adapter) + "_eng_" +
         std::to_string(index) + "_engtype_" + std::string(type);
}

// The process's use of engine type `type`, or nullopt.
std::optional<double> percent_of(const GpuProcess& process, std::string_view type) {
  const auto found = std::ranges::find(process.engines, type, &GpuEngineUse::type);
  return found == process.engines.end() ? std::nullopt : std::optional<double>{found->percent};
}

class GpuCollectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<GpuCollector>> opened = GpuCollector::open(counters_);
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    collector_ = std::move(*opened);
  }

  std::map<SeriesKey, double> sample() {
    const Result<std::vector<Measurement>> measured = collector_->collect(WallTime{}, MonoTime{});
    EXPECT_TRUE(measured.has_value());
    std::map<SeriesKey, double> values;
    for (const Measurement& measurement : measured.value_or(std::vector<Measurement>{})) {
      values[measurement.key] = measurement.value;
    }
    return values;
  }

  [[nodiscard]] const GpuProcess* process(std::uint32_t pid) const {
    const std::shared_ptr<const GpuView> view = collector_->latest();
    const auto found = std::ranges::find(view->processes, pid, &GpuProcess::pid);
    return found == view->processes.end() ? nullptr : &*found;
  }

  testing::FakeCounters& counters() { return counters_; }

 private:
  testing::FakeCounters counters_;
  std::unique_ptr<GpuCollector> collector_;
};

TEST_F(GpuCollectorTest, AGameHasItsOwn3dUtilizationAndDedicatedMemory) {
  counters().set(engines, {{.instance = engine(4242, 0, "3D"), .value = 71.0},
                           {.instance = engine(500, 0, "3D"), .value = 4.0},
                           {.instance = engine(4242, 5, "Video Decode"), .value = 12.0}});
  counters().set(R"(\GPU Process Memory(*)\Dedicated Usage)",
                 {{.instance = "pid_4242_" + std::string(adapter), .value = 3.0e9}});
  counters().set(R"(\GPU Process Memory(*)\Shared Usage)",
                 {{.instance = "pid_4242_" + std::string(adapter), .value = 2.0e8}});
  static_cast<void>(sample());

  const GpuProcess* game = process(4242);
  ASSERT_NE(game, nullptr);
  EXPECT_EQ(percent_of(*game, "3D"), 71.0);
  EXPECT_EQ(percent_of(*game, "Video Decode"), 12.0);  // engine types may contain spaces
  EXPECT_EQ(game->dedicated, Bytes{3'000'000'000});
  EXPECT_EQ(game->shared, Bytes{200'000'000});
  ASSERT_NE(process(500), nullptr);
  EXPECT_EQ(percent_of(*process(500), "3D"), 4.0);
}

TEST_F(GpuCollectorTest, AnEngineSumsItsProcessesAndATypeShowsItsBusiestEngine) {
  counters().set(engines, {{.instance = engine(1, 0, "3D"), .value = 60.0},
                           {.instance = engine(2, 0, "3D"), .value = 30.0},
                           {.instance = engine(3, 1, "3D"), .value = 95.0},
                           {.instance = engine(3, 2, "Copy"), .value = 70.0},
                           {.instance = engine(4, 2, "Copy"), .value = 50.0},
                           {.instance = "garbage", .value = 99.0},
                           {.instance = "pid_x_luid_eng_0_engtype_3D", .value = 99.0}});
  counters().set(R"(\GPU Adapter Memory(*)\Dedicated Usage)",
                 {{.instance = std::string(adapter), .value = 6.0e9}});
  counters().set(R"(\GPU Adapter Memory(*)\Shared Usage)",
                 {{.instance = std::string(adapter), .value = 1.0e9}});
  const std::map<SeriesKey, double> values = sample();

  const auto value = [&](std::string_view name, const std::string& entity) {
    const auto found = values.find(SeriesKey{.metric = std::string(name), .entity = entity});
    return found == values.end() ? std::nullopt : std::optional<double>{found->second};
  };
  const std::string id(adapter);
  EXPECT_EQ(value(metric::gpu_engine, id + "/3D"), 95.0);     // max(60 + 30, 95)
  EXPECT_EQ(value(metric::gpu_engine, id + "/Copy"), 100.0);  // 70 + 50, capped
  EXPECT_EQ(value(metric::gpu_utilization, id), 100.0);
  EXPECT_EQ(value(metric::gpu_dedicated, id), 6.0e9);
  EXPECT_EQ(value(metric::gpu_shared, id), 1.0e9);
  EXPECT_EQ(values.size(), 5U);  // malformed instances are ignored
}

TEST(GpuCollectorOpen, FailsWhenTheMachineHasNoGpuCounters) {
  testing::FakeCounters counters;
  counters.remove(engines);
  const Result<std::unique_ptr<GpuCollector>> opened = GpuCollector::open(counters);
  ASSERT_FALSE(opened.has_value());
  EXPECT_NE(opened.error().message.find("GPU Engine"), std::string::npos);
}

}  // namespace
}  // namespace myclean::telemetry
