#include "platform/perf/pdh_counters.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/gpu_collector.hpp"
#include "core/telemetry/memory_status.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/system_collectors.hpp"
#include "platform/perf/win32_memory_status.hpp"

namespace myclean::platform {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using telemetry::CounterValue;
using telemetry::ICollector;
using telemetry::Measurement;

// PDH computes a rate from two readings some time apart.
constexpr auto reading_gap = 300ms;

std::unique_ptr<telemetry::ICounterQuery> open_query(PdhCounters& counters) {
  Result<std::unique_ptr<telemetry::ICounterQuery>> query = counters.open();
  EXPECT_TRUE(query.has_value()) << query.error().message;
  return query ? std::move(*query) : nullptr;
}

// Two samples `reading_gap` apart; the measurements of the second.
std::vector<Measurement> sample_twice(ICollector& collector) {
  EXPECT_TRUE(collector.collect(std::chrono::system_clock::now(), {}).has_value());
  std::this_thread::sleep_for(reading_gap);
  Result<std::vector<Measurement>> second = collector.collect(std::chrono::system_clock::now(), {});
  EXPECT_TRUE(second.has_value()) << second.error().message;
  return second.value_or(std::vector<Measurement>{});
}

TEST(PdhCounters, ARateCounterHasValuesForEveryLogicalProcessorFromItsSecondReading) {
  PdhCounters counters;
  const std::unique_ptr<telemetry::ICounterQuery> query = open_query(counters);
  ASSERT_NE(query, nullptr);
  const Result<std::size_t> utility =
      query->add(R"(\Processor Information(*)\% Processor Utility)");
  ASSERT_TRUE(utility.has_value()) << utility.error().message;

  ASSERT_TRUE(query->collect().has_value());
  const Result<std::vector<CounterValue>> first = query->values(*utility);
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_TRUE(first->empty());

  std::this_thread::sleep_for(reading_gap);
  ASSERT_TRUE(query->collect().has_value());
  const Result<std::vector<CounterValue>> second = query->values(*utility);
  ASSERT_TRUE(second.has_value()) << second.error().message;
  // Every logical processor, plus one total per processor group and the overall total.
  EXPECT_GE(second->size(), std::thread::hardware_concurrency() + 2);
  for (const CounterValue& value : *second) {
    EXPECT_GE(value.value, 0.0) << value.instance;
    EXPECT_LE(value.value, 300.0) << value.instance;  // utility exceeds 100 % while boosting
  }
}

TEST(PdhCounters, ACounterWithoutInstancesHasOneValueWithAnEmptyName) {
  PdhCounters counters;
  const std::unique_ptr<telemetry::ICounterQuery> query = open_query(counters);
  ASSERT_NE(query, nullptr);
  const Result<std::size_t> modified = query->add(R"(\Memory\Modified Page List Bytes)");
  ASSERT_TRUE(modified.has_value()) << modified.error().message;
  ASSERT_TRUE(query->collect().has_value());
  const Result<std::vector<CounterValue>> values = query->values(*modified);
  ASSERT_TRUE(values.has_value()) << values.error().message;
  ASSERT_EQ(values->size(), 1U);
  EXPECT_EQ(values->front().instance, "");
  EXPECT_GT(values->front().value, 0.0);
}

TEST(PdhCounters, AnUnknownCounterIsNotFound) {
  PdhCounters counters;
  const std::unique_ptr<telemetry::ICounterQuery> query = open_query(counters);
  ASSERT_NE(query, nullptr);
  const Result<std::size_t> missing = query->add(R"(\No Such Object(*)\No Such Counter)");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

TEST(Win32MemoryStatus, CommitIsWithinItsLimitAndAvailableWithinTotal) {
  const Win32MemoryStatus memory;
  const Result<telemetry::MemoryStatus> status = memory.read();
  ASSERT_TRUE(status.has_value()) << status.error().message;
  EXPECT_GE(status->total, 1_GiB);
  EXPECT_GT(status->available, Bytes{0});
  EXPECT_LE(status->available, status->total);
  EXPECT_GT(status->commit, Bytes{0});
  EXPECT_LE(status->commit, status->commit_limit);
  EXPECT_GT(status->pool_nonpaged, Bytes{0});
}

TEST(SystemCollectors, CpuMemoryAndDiskSeriesHavePlausibleValues) {
  PdhCounters counters;
  const Win32MemoryStatus memory;
  std::map<std::string, std::set<std::string>> entities;  // metric -> entities
  std::vector<Result<std::unique_ptr<ICollector>>> collectors;
  collectors.push_back(telemetry::make_cpu_collector(counters));
  collectors.push_back(telemetry::make_memory_collector(memory, counters));
  collectors.push_back(telemetry::make_disk_collector(counters));
  for (Result<std::unique_ptr<ICollector>>& collector : collectors) {
    ASSERT_TRUE(collector.has_value()) << collector.error().message;
    for (const Measurement& measurement : sample_twice(**collector)) {
      entities[measurement.key.metric].insert(measurement.key.entity);
      EXPECT_GE(measurement.value, 0.0) << measurement.key.metric << " " << measurement.key.entity;
      if (measurement.key.metric == telemetry::metric::cpu_utility ||
          measurement.key.metric == telemetry::metric::disk_idle) {
        EXPECT_LE(measurement.value, 100.0);
      }
    }
  }
  EXPECT_EQ(entities[std::string(telemetry::metric::cpu_utility)].size(),
            std::thread::hardware_concurrency() + 1);
  EXPECT_EQ(entities[std::string(telemetry::metric::memory_hard_faults)].size(), 1U);
  EXPECT_EQ(entities[std::string(telemetry::metric::memory_standby)].size(), 1U);
  const std::set<std::string>& disks = entities[std::string(telemetry::metric::disk_idle)];
  EXPECT_FALSE(disks.empty());
  std::cout << "physical disks discovered: " << disks.size() << " (";
  for (const std::string& disk : disks) {
    std::cout << " " << disk;
  }
  std::cout << " )\n";
}

TEST(GpuCollector, EngineUtilizationIsAPercentageWhenTheMachineHasAGpu) {
  PdhCounters counters;
  Result<std::unique_ptr<telemetry::GpuCollector>> gpu = telemetry::GpuCollector::open(counters);
  if (!gpu) {
    GTEST_SKIP() << "no GPU counters on this machine: " << gpu.error().message;
  }
  std::set<std::string> adapters;
  for (const Measurement& measurement : sample_twice(**gpu)) {
    EXPECT_GE(measurement.value, 0.0) << measurement.key.metric << " " << measurement.key.entity;
    if (measurement.key.metric == telemetry::metric::gpu_utilization) {
      adapters.insert(measurement.key.entity);
      EXPECT_LE(measurement.value, 100.0);
    }
  }
  ASSERT_NE((*gpu)->latest(), nullptr);
  std::cout << "GPU adapters with engine use: " << adapters.size()
            << ", processes using a GPU: " << (*gpu)->latest()->processes.size() << "\n";
}

}  // namespace
}  // namespace myclean::platform
