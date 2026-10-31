#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/process_table.hpp"

namespace myclean::telemetry {

struct ProcessRow {
  ProcessInfo info;
  // Index in ProcessView::rows of the parent, set only when the parent PID belongs to a process
  // created before this one (so a reused PID is never taken for the parent) and the parent is
  // not a launcher (see ProcessCollector).
  std::optional<std::size_t> parent;
  double cpu_percent = 0.0;  // of all logical processors since the previous sample
  double io_bytes_per_second = 0.0;
};

// A root process and its descendants, with totals over all of them.
struct ProcessTree {
  std::vector<std::size_t> members;  // indices in ProcessView::rows, the root first
  double cpu_percent = 0.0;
  Bytes private_bytes;
  Bytes private_working_set;
  Bytes working_set;
  double io_bytes_per_second = 0.0;
  std::uint64_t handles = 0;
  std::uint64_t threads = 0;
};

struct ProcessView {
  WallTime time;
  std::vector<ProcessRow> rows;
  std::vector<ProcessTree> trees;  // highest CPU first
};

// Samples the process table every second: per-process CPU % (Task Manager semantics: share of
// all logical processors) and I/O rate from the counters' change since the previous sample, and
// process trees. The first sample has no previous one, so its rates are 0.
//
// Children of launcher processes (explorer, services, svchost, wininit, winlogon) start trees of
// their own: otherwise every app the user starts would join one explorer tree.
//
// Records process.count/threads/handles, memory.compressed and cpu.queue (see metrics.hpp); the
// full table is available through latest(). cpu.queue, the "Processor Queue Length" counter, is
// counted from this snapshot's ready threads: the System counter object would walk every thread
// of the machine a second time.
class ProcessCollector final : public ICollector {
 public:
  // `table` must outlive the collector.
  explicit ProcessCollector(IProcessTable& table);

  [[nodiscard]] std::string_view name() const override { return "processes"; }
  [[nodiscard]] std::chrono::seconds interval() const override { return std::chrono::seconds{1}; }
  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime now, MonoTime monotonic) override;

  // The newest table; null before the first sample. Thread-safe.
  [[nodiscard]] std::shared_ptr<const ProcessView> latest() const { return latest_.get(); }

 private:
  // A process instance: PIDs are reused, a PID with its creation time is not.
  struct Key {
    std::uint32_t pid = 0;
    WallTime created;
    friend auto operator<=>(const Key&, const Key&) = default;
  };
  struct Counters {
    std::chrono::nanoseconds cpu_time{};
    std::uint64_t io_bytes = 0;
  };

  IProcessTable* table_;
  std::map<Key, Counters> previous_;
  std::optional<MonoTime> previous_time_;
  Latest<ProcessView> latest_;
};

}  // namespace myclean::telemetry
