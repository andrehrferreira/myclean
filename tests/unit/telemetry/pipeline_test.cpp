#include "core/telemetry/pipeline.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/registration.hpp"
#include "core/telemetry/volume_info.hpp"
#include "fake_sources.hpp"
#include "fakes/fake_clock.hpp"
#include "temp_store.hpp"

namespace myclean::telemetry {
namespace {

using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 12h;

// What a test sees of, and controls in, a CountingCollector.
struct Probe {
  int samples = 0;
  bool failing = false;
};

// Counts its samples and records the count; fails while its probe says so.
class CountingCollector final : public ICollector {
 public:
  CountingCollector(std::string name, std::chrono::seconds interval, Probe& probe)
      : name_(std::move(name)), interval_(interval), probe_(&probe) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  [[nodiscard]] std::chrono::seconds interval() const override { return interval_; }
  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime /*now*/,
                                                         MonoTime /*monotonic*/) override {
    ++probe_->samples;
    if (probe_->failing) {
      return fail(Errc::io_error, "source is gone");
    }
    return std::vector<Measurement>{
        measure("test.count", name_, static_cast<double>(probe_->samples))};
  }

 private:
  std::string name_;
  std::chrono::seconds interval_;
  Probe* probe_;
};

class PipelineTest : public ::testing::Test {
 protected:
  // Adds a counting collector and returns its probe.
  Probe& add(const std::string& name, std::chrono::seconds interval) {
    Probe& probe = probes_[name];
    pipeline_.add(std::make_unique<CountingCollector>(name, interval, probe));
    return probe;
  }

  std::vector<Sample> recorded(const std::string& entity) {
    return store_.get().raw({.metric = "test.count", .entity = entity});
  }

  fakes::FakeClock& clock() { return clock_; }
  store::Store& store() { return store_.get(); }
  Pipeline& pipeline() { return pipeline_; }

 private:
  fakes::FakeClock clock_{noon};
  testing::TempStore store_{clock_};
  std::map<std::string, Probe> probes_;  // outlives the pipeline's collectors
  Pipeline pipeline_{clock_, store_.get()};
};

TEST_F(PipelineTest, EachCollectorRunsAtItsOwnIntervalAndItsSamplesReachTheStore) {
  const Probe& fast = add("fast", 1s);
  const Probe& slow = add("slow", 60s);

  for (int second = 0; second < 150; ++second) {
    EXPECT_TRUE(pipeline().poll().empty());
    clock().advance(1s);
  }
  EXPECT_EQ(fast.samples, 150);
  EXPECT_EQ(slow.samples, 3);  // at 0, 60 and 120 s
  ASSERT_EQ(recorded("slow").size(), 3U);
  EXPECT_EQ(recorded("slow")[1].time, noon + 60s);
  EXPECT_EQ(recorded("fast").size(), 150U);
}

TEST_F(PipelineTest, PollRunsOnlyWhatIsDueAndNextDueSaysWhen) {
  EXPECT_EQ(pipeline().next_due(), std::nullopt);
  const Probe& fast = add("fast", 1s);
  const Probe& slow = add("slow", 60s);
  EXPECT_LE(pipeline().next_due(), clock().monotonic());

  static_cast<void>(pipeline().poll());
  EXPECT_EQ(pipeline().next_due(), clock().monotonic() + 1s);
  clock().advance(500ms);
  static_cast<void>(pipeline().poll());
  EXPECT_EQ(fast.samples, 1);
  EXPECT_EQ(slow.samples, 1);
}

TEST_F(PipelineTest, IntervalsMissedWhileStalledAreSkippedNotCaughtUp) {
  const Probe& fast = add("fast", 1s);
  static_cast<void>(pipeline().poll());
  clock().advance(30s);  // the machine slept
  static_cast<void>(pipeline().poll());
  static_cast<void>(pipeline().poll());
  EXPECT_EQ(fast.samples, 2);
  EXPECT_EQ(pipeline().next_due(), clock().monotonic() + 1s);
}

TEST_F(PipelineTest, AFailingCollectorIsReportedByNameAndDoesNotStopTheOthers) {
  Probe& broken = add("broken", 1s);
  add("healthy", 1s);
  broken.failing = true;

  const std::vector<Error> errors = pipeline().poll();
  ASSERT_EQ(errors.size(), 1U);
  EXPECT_EQ(errors[0].message, "broken: source is gone");
  EXPECT_EQ(recorded("healthy").size(), 1U);

  broken.failing = false;
  clock().advance(1s);
  EXPECT_TRUE(pipeline().poll().empty());
  EXPECT_EQ(recorded("broken").size(), 1U);
}

TEST_F(PipelineTest, RegistrationAddsEveryCollectorAndReportsTheUnavailableOnes) {
  testing::FakeProcessTable processes;
  processes.processes() = {ProcessInfo{.pid = 4, .image = "System", .threads = 200}};
  testing::FakeCounters counters;
  counters.remove(R"(\GPU Engine(*)\Utilization Percentage)");
  counters.set(R"(\Processor Information(*)\% Processor Utility)",
               {{.instance = "_Total", .value = 12.0}});
  const testing::FakeMemoryStatus memory;
  const testing::FakeVolumeInfo volumes{
      std::vector<VolumeStatus>{{.id = "v", .mount_points = {R"(C:\)"}, .disks = {0}}}};

  const Registration registration =
      register_collectors(pipeline(), processes, counters, memory, volumes);
  ASSERT_EQ(registration.unavailable.size(), 1U);
  EXPECT_TRUE(registration.unavailable[0].message.starts_with("gpu: "));
  EXPECT_EQ(registration.live.gpu, nullptr);
  ASSERT_NE(registration.live.processes, nullptr);
  ASSERT_NE(registration.live.volumes, nullptr);

  EXPECT_TRUE(pipeline().poll().empty());
  EXPECT_EQ(counters.collects(), 3);  // cpu, memory and disks
  EXPECT_EQ(registration.live.processes->latest()->rows.size(), 1U);
  EXPECT_EQ(registration.live.volumes->latest()->volumes.size(), 1U);
  const auto has = [&](std::string_view name, const std::string& entity) {
    return !store().raw({.metric = std::string(name), .entity = entity}).empty();
  };
  EXPECT_TRUE(has(metric::cpu_utility, "system"));
  EXPECT_TRUE(has(metric::cpu_queue, "system"));
  EXPECT_TRUE(has(metric::memory_commit, "system"));
  EXPECT_TRUE(has(metric::process_threads, "system"));
  EXPECT_TRUE(has(metric::volume_free, "C:"));
}

}  // namespace
}  // namespace myclean::telemetry
