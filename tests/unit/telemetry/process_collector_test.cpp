#include "core/telemetry/process_collector.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/process_table.hpp"
#include "fake_sources.hpp"

namespace myclean::telemetry {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime boot = std::chrono::sys_days{std::chrono::October / 31 / 2026};

ProcessInfo process(std::uint32_t pid, std::uint32_t parent, std::chrono::seconds created,
                    std::string image) {
  return ProcessInfo{.pid = pid,
                     .parent_pid = parent,
                     .created = boot + created,
                     .image = std::move(image),
                     .private_bytes = 100_MiB,
                     .working_set = 150_MiB,
                     .private_working_set = 80_MiB,
                     .handles = 200,
                     .threads = 10};
}

const ProcessRow& row_of(const ProcessView& view, std::uint32_t pid) {
  const auto found =
      std::ranges::find(view.rows, pid, [](const ProcessRow& row) { return row.info.pid; });
  EXPECT_NE(found, view.rows.end()) << "no row for PID " << pid;
  return *found;
}

// The tree whose root has `pid`.
const ProcessTree* tree_of(const ProcessView& view, std::uint32_t pid) {
  for (const ProcessTree& tree : view.trees) {
    if (view.rows[tree.members.front()].info.pid == pid) {
      return &tree;
    }
  }
  return nullptr;
}

std::optional<double> value_of(const std::vector<Measurement>& measurements,
                               std::string_view metric) {
  for (const Measurement& measurement : measurements) {
    if (measurement.key.metric == metric) {
      return measurement.value;
    }
  }
  return std::nullopt;
}

class ProcessCollectorTest : public ::testing::Test {
 protected:
  std::shared_ptr<const ProcessView> sample() {
    const Result<std::vector<Measurement>> measured = collector_.collect(boot + elapsed_, mono_);
    EXPECT_TRUE(measured.has_value());
    last_ = measured ? *measured : std::vector<Measurement>{};
    return collector_.latest();
  }
  void advance(std::chrono::seconds duration) {
    elapsed_ += duration;
    mono_ += duration;
  }
  std::vector<ProcessInfo>& processes() { return table_.processes(); }
  [[nodiscard]] const ProcessCollector& collector() const { return collector_; }
  // The measurements of the last sample.
  [[nodiscard]] const std::vector<Measurement>& last() const { return last_; }

 private:
  testing::FakeProcessTable table_{32};
  ProcessCollector collector_{table_};
  std::vector<Measurement> last_;
  std::chrono::seconds elapsed_{3600};
  MonoTime mono_{std::chrono::hours{1}};
};

TEST_F(ProcessCollectorTest, NothingIsPublishedBeforeTheFirstSample) {
  EXPECT_EQ(collector().latest(), nullptr);
}

TEST_F(ProcessCollectorTest, CpuIsAShareOfAllLogicalProcessorsFromTheChangeSinceTheLastSample) {
  processes() = {process(100, 4, 10s, "build.exe")};
  processes()[0].cpu_time = 50s;
  processes()[0].io_bytes = 1000;
  const std::shared_ptr<const ProcessView> first = sample();
  EXPECT_EQ(row_of(*first, 100).cpu_percent, 0.0);  // no previous sample yet
  EXPECT_EQ(row_of(*first, 100).io_bytes_per_second, 0.0);

  // 8 of 32 logical processors busy for 2 s: 16 s of CPU time is 25 % of the machine.
  advance(2s);
  processes()[0].cpu_time = 66s;
  processes()[0].io_bytes = 5000;
  const std::shared_ptr<const ProcessView> second = sample();
  EXPECT_DOUBLE_EQ(row_of(*second, 100).cpu_percent, 25.0);
  EXPECT_DOUBLE_EQ(row_of(*second, 100).io_bytes_per_second, 2000.0);
}

TEST_F(ProcessCollectorTest, AReusedPidStartsFromZeroInsteadOfTheOldProcessCounters) {
  processes() = {process(100, 4, 10s, "old.exe")};
  processes()[0].cpu_time = 500s;
  static_cast<void>(sample());
  advance(1s);
  processes() = {process(100, 4, 3000s, "new.exe")};
  processes()[0].cpu_time = 1s;
  EXPECT_EQ(row_of(*sample(), 100).cpu_percent, 0.0);
}

TEST_F(ProcessCollectorTest, BrowserAndItsThirtyChildrenFormOneTreeWithSummedTotals) {
  processes() = {process(1000, 900, 50s, "explorer.exe"), process(2000, 1000, 100s, "browser.exe")};
  for (std::uint32_t child = 0; child < 30; ++child) {
    processes().push_back(process(3000 + child, 2000, 200s, "browser.exe"));
  }
  processes().push_back(process(4000, 3000, 300s, "renderer-helper.exe"));  // grandchild
  const std::shared_ptr<const ProcessView> view = sample();

  const ProcessTree* browser = tree_of(*view, 2000);
  ASSERT_NE(browser, nullptr);
  EXPECT_EQ(browser->members.size(), 32U);
  EXPECT_EQ(browser->private_bytes, Bytes{32 * (100_MiB).count()});
  EXPECT_EQ(browser->working_set, Bytes{32 * (150_MiB).count()});
  EXPECT_EQ(browser->private_working_set, Bytes{32 * (80_MiB).count()});
  EXPECT_EQ(browser->handles, 32U * 200U);
  EXPECT_EQ(browser->threads, 32U * 10U);
  // explorer.exe launches apps; its children are not part of its tree.
  ASSERT_NE(tree_of(*view, 1000), nullptr);
  EXPECT_EQ(tree_of(*view, 1000)->members.size(), 1U);
}

TEST_F(ProcessCollectorTest, ParentPidReusedByANewerProcessIsNotTakenForTheParent) {
  // 500's parent 400 exited; PID 400 now belongs to a process created after 500.
  processes() = {process(500, 400, 100s, "orphan.exe"), process(400, 4, 200s, "newer.exe")};
  const std::shared_ptr<const ProcessView> view = sample();

  EXPECT_FALSE(row_of(*view, 500).parent.has_value());
  ASSERT_NE(tree_of(*view, 500), nullptr);
  EXPECT_EQ(tree_of(*view, 500)->members.size(), 1U);
  EXPECT_EQ(tree_of(*view, 400)->members.size(), 1U);
}

TEST_F(ProcessCollectorTest, TreesAreOrderedByCpuAndSumTheirMembersCpu) {
  processes() = {process(10, 4, 10s, "idle-app.exe"), process(20, 4, 10s, "busy.exe"),
                 process(21, 20, 20s, "worker.exe")};
  static_cast<void>(sample());
  advance(1s);
  processes()[1].cpu_time = 4s;  // 4 of 32 processors: 12.5 %
  processes()[2].cpu_time = 8s;  // 25 %
  const std::shared_ptr<const ProcessView> view = sample();

  ASSERT_EQ(view->trees.size(), 2U);
  EXPECT_EQ(view->rows[view->trees[0].members.front()].info.pid, 20U);
  EXPECT_DOUBLE_EQ(view->trees[0].cpu_percent, 37.5);
}

TEST_F(ProcessCollectorTest, RecordsTotalsTheProcessorQueueAndTheCompressedMemoryStore) {
  processes() = {process(0, 0, 0s, "System Idle Process"), process(4, 0, 0s, "System"),
                 process(300, 4, 5s, "Memory Compression")};
  processes()[2].working_set = 2_GiB;
  processes()[1].ready_threads = 3;
  processes()[2].ready_threads = 2;
  static_cast<void>(sample());

  EXPECT_EQ(value_of(last(), metric::process_count), 3.0);
  EXPECT_EQ(value_of(last(), metric::process_threads), 30.0);
  EXPECT_EQ(value_of(last(), metric::process_handles), 600.0);
  EXPECT_EQ(value_of(last(), metric::memory_compressed), static_cast<double>((2_GiB).count()));
  EXPECT_EQ(value_of(last(), metric::cpu_queue), 5.0);
  for (const Measurement& measurement : last()) {
    EXPECT_EQ(measurement.key.entity, metric::system);
  }
}

TEST_F(ProcessCollectorTest, RecordsMyCleansOwnCpuAndMemory) {
  processes() = {process(700, 600, 10s, "myclean-svc.exe"),
                 process(800, 1000, 20s, "myclean-tray.exe"), process(900, 4, 30s, "other.exe")};
  processes()[1].private_working_set = 20_MiB;
  static_cast<void>(sample());
  // 0.32 s of 32 processors over 1 s: 1 %.
  advance(1s);
  processes()[0].cpu_time = 320ms;
  static_cast<void>(sample());

  std::map<std::pair<std::string, std::string>, double> self;
  for (const Measurement& measurement : last()) {
    if (measurement.key.metric.starts_with("self.")) {
      self[{measurement.key.metric, measurement.key.entity}] = measurement.value;
    }
  }
  ASSERT_EQ(self.size(), 4U);  // other.exe is not recorded
  EXPECT_DOUBLE_EQ((self[{std::string(metric::self_cpu), "myclean-svc"}]), 1.0);
  EXPECT_DOUBLE_EQ((self[{std::string(metric::self_cpu), "myclean-tray"}]), 0.0);
  EXPECT_EQ((self[{std::string(metric::self_memory), "myclean-svc"}]),
            static_cast<double>((80_MiB).count()));
  EXPECT_EQ((self[{std::string(metric::self_memory), "myclean-tray"}]),
            static_cast<double>((20_MiB).count()));
}

}  // namespace
}  // namespace myclean::telemetry
