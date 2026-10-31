#include "core/telemetry/system_collectors.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/pipeline.hpp"
#include "fake_sources.hpp"
#include "fakes/fake_clock.hpp"
#include "temp_store.hpp"

namespace myclean::telemetry {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

// Every measurement by series.
std::map<SeriesKey, double> sample(ICollector& collector) {
  const Result<std::vector<Measurement>> measured = collector.collect(noon, MonoTime{});
  EXPECT_TRUE(measured.has_value());
  std::map<SeriesKey, double> values;
  for (const Measurement& measurement : measured.value_or(std::vector<Measurement>{})) {
    values[measurement.key] = measurement.value;
  }
  return values;
}

std::optional<double> find(const std::map<SeriesKey, double>& values, std::string_view metric,
                           std::string_view entity) {
  const auto found =
      values.find(SeriesKey{.metric = std::string(metric), .entity = std::string(entity)});
  return found == values.end() ? std::nullopt : std::optional<double>{found->second};
}

std::unique_ptr<ICollector> open(Result<std::unique_ptr<ICollector>> collector) {
  EXPECT_TRUE(collector.has_value()) << collector.error().message;
  return collector ? std::move(*collector) : nullptr;
}

// "0,0" ... "0,31", "0,_Total" and "_Total", all with `value`.
std::vector<CounterValue> processors(double value) {
  std::vector<CounterValue> values;
  values.reserve(34);
  for (int processor = 0; processor < 32; ++processor) {
    values.push_back({.instance = "0," + std::to_string(processor), .value = value});
  }
  values.push_back({.instance = "0,_Total", .value = value});
  values.push_back({.instance = "_Total", .value = value});
  return values;
}

TEST(CpuCollector, EveryLogicalProcessorAndTheTotalGetASampleEverySecond) {
  fakes::FakeClock clock{noon};
  testing::TempStore store{clock};
  testing::FakeCounters counters;
  counters.set(R"(\Processor Information(*)\% Processor Utility)", processors(40.0));
  counters.set(R"(\Processor Information(*)\% of Maximum Frequency)", processors(97.0));
  Pipeline pipeline{clock, store.get()};
  pipeline.add(open(make_cpu_collector(counters)));

  for (int second = 0; second < 10; ++second) {
    EXPECT_TRUE(pipeline.poll().empty());
    clock.advance(1s);
  }

  std::set<std::string> entities{std::string(metric::system)};
  for (int processor = 0; processor < 32; ++processor) {
    entities.insert("0," + std::to_string(processor));
  }
  for (const std::string& entity : entities) {
    for (const std::string_view name : {metric::cpu_utility, metric::cpu_frequency}) {
      EXPECT_EQ(store.get().raw({.metric = std::string(name), .entity = entity}).size(), 10U)
          << name << " " << entity;
    }
  }
  EXPECT_TRUE(
      store.get().raw({.metric = std::string(metric::cpu_utility), .entity = "0,_Total"}).empty());
}

TEST(CpuCollector, ThrottlingShowsBelowMaximumFrequencyAndUtilityIsCappedLikeTaskManager) {
  testing::FakeCounters counters;
  counters.set(R"(\Processor Information(*)\% Processor Utility)",
               {{.instance = "0,3", .value = 131.0}, {.instance = "_Total", .value = 20.0}});
  counters.set(R"(\Processor Information(*)\% of Maximum Frequency)",
               {{.instance = "0,3", .value = 62.0}});
  counters.set(R"(\Processor Information(_Total)\% DPC Time)",
               {{.instance = "_Total", .value = 1.5}});
  counters.set(R"(\Processor Information(_Total)\% Interrupt Time)",
               {{.instance = "_Total", .value = 0.5}});
  const std::unique_ptr<ICollector> cpu = open(make_cpu_collector(counters));
  const std::map<SeriesKey, double> values = sample(*cpu);

  EXPECT_EQ(find(values, metric::cpu_frequency, "0,3"), 62.0);
  EXPECT_EQ(find(values, metric::cpu_utility, "0,3"), 100.0);
  EXPECT_EQ(find(values, metric::cpu_utility, metric::system), 20.0);
  EXPECT_EQ(find(values, metric::cpu_dpc, metric::system), 1.5);
  EXPECT_EQ(find(values, metric::cpu_interrupt, metric::system), 0.5);
}

TEST(DiskCollector, EveryDiskTheCountersReportIsDiscoveredWithoutHardCodedNumbers) {
  testing::FakeCounters counters;
  const std::vector<CounterValue> instances{
      {.instance = "0 C:", .value = 0.004},  {.instance = "1 D: E:", .value = 0.004},
      {.instance = "2", .value = 0.004},     {.instance = "3 F:", .value = 0.004},
      {.instance = "4 G:", .value = 0.004},  {.instance = "5 H:", .value = 0.004},
      {.instance = "6 I:", .value = 0.004},  {.instance = "7 J:", .value = 0.004},
      {.instance = "_Total", .value = 0.004}};
  for (const std::string_view counter :
       {R"(\PhysicalDisk(*)\Disk Read Bytes/sec)", R"(\PhysicalDisk(*)\Disk Write Bytes/sec)",
        R"(\PhysicalDisk(*)\Disk Transfers/sec)", R"(\PhysicalDisk(*)\Avg. Disk sec/Read)",
        R"(\PhysicalDisk(*)\Avg. Disk sec/Write)", R"(\PhysicalDisk(*)\Current Disk Queue Length)",
        R"(\PhysicalDisk(*)\% Idle Time)"}) {
    counters.set(counter, instances);
  }
  counters.set(R"(\PhysicalDisk(*)\% Idle Time)", {{.instance = "2", .value = 104.0}});
  const std::unique_ptr<ICollector> disks = open(make_disk_collector(counters));
  const std::map<SeriesKey, double> values = sample(*disks);

  std::set<std::string> found;
  for (const auto& [key, value] : values) {
    if (key.metric == metric::disk_read_latency) {
      found.insert(key.entity);
      EXPECT_DOUBLE_EQ(value, 4.0);  // milliseconds
    }
  }
  EXPECT_EQ(found, (std::set<std::string>{"0", "1", "2", "3", "4", "5", "6", "7"}));
  EXPECT_EQ(find(values, metric::disk_idle, "2"), 100.0);
  EXPECT_EQ(find(values, metric::disk_write_bytes, "1"), 0.004);
}

TEST(DiskCollector, FailsToOpenNamingTheCounterThatIsMissing) {
  testing::FakeCounters counters;
  counters.remove(R"(\PhysicalDisk(*)\% Idle Time)");
  const Result<std::unique_ptr<ICollector>> disks = make_disk_collector(counters);
  ASSERT_FALSE(disks.has_value());
  EXPECT_EQ(disks.error().code, Errc::not_found);
  EXPECT_NE(disks.error().message.find("% Idle Time"), std::string::npos);
}

TEST(MemoryCollector, CommitAndItsLimitComeFromOneReadingAndStandbySumsItsBands) {
  const testing::FakeMemoryStatus memory{MemoryStatus{.total = 128_GiB,
                                                      .available = 40_GiB,
                                                      .commit = 90_GiB,
                                                      .commit_limit = 150_GiB,
                                                      .pool_paged = 1_GiB,
                                                      .pool_nonpaged = 512_MiB}};
  testing::FakeCounters counters;
  counters.set(R"(\Memory\Standby Cache Core Bytes)", 1.0e9);
  counters.set(R"(\Memory\Standby Cache Normal Priority Bytes)", 2.0e9);
  counters.set(R"(\Memory\Standby Cache Reserve Bytes)", 3.0e9);
  counters.set(R"(\Memory\Modified Page List Bytes)", 5.0e8);
  const std::unique_ptr<ICollector> collector = open(make_memory_collector(memory, counters));
  const std::map<SeriesKey, double> values = sample(*collector);

  const auto bytes = [](Bytes value) { return static_cast<double>(value.count()); };
  EXPECT_EQ(find(values, metric::memory_commit, metric::system), bytes(90_GiB));
  EXPECT_EQ(find(values, metric::memory_commit_limit, metric::system), bytes(150_GiB));
  EXPECT_EQ(find(values, metric::memory_available, metric::system), bytes(40_GiB));
  EXPECT_EQ(find(values, metric::memory_total, metric::system), bytes(128_GiB));
  EXPECT_EQ(find(values, metric::memory_pool_nonpaged, metric::system), bytes(512_MiB));
  EXPECT_EQ(find(values, metric::memory_standby, metric::system), 6.0e9);
  EXPECT_EQ(find(values, metric::memory_modified, metric::system), 5.0e8);
  // Pages Input/sec is a rate: no value before its second reading.
  EXPECT_EQ(find(values, metric::memory_hard_faults, metric::system), std::nullopt);

  counters.set(R"(\Memory\Pages Input/sec)", 250.0);
  EXPECT_EQ(find(sample(*collector), metric::memory_hard_faults, metric::system), 250.0);
}

}  // namespace
}  // namespace myclean::telemetry
