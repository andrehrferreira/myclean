#include "platform/proc/nt_process_table.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/process_collector.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::platform {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using telemetry::ProcessInfo;

const ProcessInfo* find(const std::vector<ProcessInfo>& processes, std::uint32_t pid) {
  const auto found = std::ranges::find(processes, pid, &ProcessInfo::pid);
  return found == processes.end() ? nullptr : &*found;
}

TEST(NtProcessTable, ListsThisProcessWithPlausibleValues) {
  NtProcessTable table;
  const Result<std::vector<ProcessInfo>> processes = table.snapshot();
  ASSERT_TRUE(processes.has_value()) << processes.error().message;

  const ProcessInfo* self = find(*processes, ::GetCurrentProcessId());
  ASSERT_NE(self, nullptr);
  EXPECT_EQ(self->image, "myclean_platform_proc_tests.exe");
  EXPECT_GE(self->threads, 1U);
  EXPECT_GT(self->handles, 0U);
  EXPECT_GT(self->private_bytes, 1_MiB);
  EXPECT_GT(self->working_set, 1_MiB);
  EXPECT_GT(self->private_working_set, Bytes{0});
  EXPECT_LE(self->private_working_set, self->working_set);
  const WallTime now = std::chrono::system_clock::now();
  EXPECT_LE(self->created, now);
  EXPECT_GT(self->created, now - 1h);

  // ctest started this process and is still running, so the parent link is valid.
  const ProcessInfo* parent = find(*processes, self->parent_pid);
  ASSERT_NE(parent, nullptr);
  EXPECT_LT(parent->created, self->created);
}

TEST(NtProcessTable, HasTheSystemProcessesAndUniquePids) {
  NtProcessTable table;
  const Result<std::vector<ProcessInfo>> processes = table.snapshot();
  ASSERT_TRUE(processes.has_value()) << processes.error().message;
  EXPECT_GT(processes->size(), 20U);
  ASSERT_NE(find(*processes, 0), nullptr);
  EXPECT_EQ(find(*processes, 0)->image, "System Idle Process");
  ASSERT_NE(find(*processes, 4), nullptr);
  EXPECT_EQ(find(*processes, 4)->image, "System");

  std::set<std::uint32_t> pids;
  for (const ProcessInfo& process : *processes) {
    EXPECT_TRUE(pids.insert(process.pid).second) << "duplicate PID " << process.pid;
    EXPECT_LE(process.ready_threads, process.threads) << process.image;
  }
  EXPECT_EQ(table.logical_processors(), std::thread::hardware_concurrency());
}

TEST(NtProcessTable, CollectorMeasuresThisProcessBusyLoopAsCpuShare) {
  NtProcessTable table;
  telemetry::ProcessCollector collector{table};
  ASSERT_TRUE(collector.collect(std::chrono::system_clock::now(), std::chrono::steady_clock::now())
                  .has_value());
  // Keep one thread busy for 200 ms.
  const auto until = std::chrono::steady_clock::now() + 200ms;
  for (volatile std::uint64_t spins = 0; std::chrono::steady_clock::now() < until;) {
    spins = spins + 1;
  }
  ASSERT_TRUE(collector.collect(std::chrono::system_clock::now(), std::chrono::steady_clock::now())
                  .has_value());

  const std::shared_ptr<const telemetry::ProcessView> view = collector.latest();
  ASSERT_NE(view, nullptr);
  double total = 0.0;
  for (const telemetry::ProcessRow& row : view->rows) {
    EXPECT_GE(row.cpu_percent, 0.0);
    EXPECT_LE(row.cpu_percent, 100.0);
    total += row.cpu_percent;
    if (row.info.pid == ::GetCurrentProcessId()) {
      // One busy thread of N logical processors is about 100/N %.
      EXPECT_GT(row.cpu_percent, 10.0 / table.logical_processors());
      EXPECT_LT(row.cpu_percent, 300.0 / table.logical_processors());
    }
  }
  // With the idle process the shares add up to at most the whole machine; processes that
  // started or exited between the samples are missing from the sum.
  EXPECT_GT(total, 0.0);
  EXPECT_LE(total, 101.0);
  EXPECT_FALSE(view->trees.empty());
}

}  // namespace
}  // namespace myclean::platform
