#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::sched {

enum class RunStatus {
  running,    // started and not finished yet
  succeeded,  //
  failed,     // the job reported an error, or MyClean stopped while it ran
  skipped,    // an occurrence came due while the previous run was still active
};

[[nodiscard]] std::string_view to_string(RunStatus status);
[[nodiscard]] std::optional<RunStatus> parse_run_status(std::string_view text);

// One row of the `schedule` table. Every time is a whole second.
struct ScheduleState {
  std::string name;        // unique job name, e.g. "monthly-report"
  std::string expression;  // the cron expression as registered
  std::optional<WallTime> last_run;
  std::optional<RunStatus> last_result;  // running, succeeded or failed; never skipped
  WallTime next_run;

  friend bool operator==(const ScheduleState&, const ScheduleState&) = default;
};

// One run of a job, or one occurrence skipped because the previous run was still active.
struct RunRecord {
  std::string job;
  WallTime started;       // whole second; for a skip, when the occurrence was skipped
  bool catch_up = false;  // the occurrence was missed (MyClean off, asleep) and run late
  RunStatus status = RunStatus::succeeded;  // succeeded, failed or skipped
  std::string detail;                       // the failure or skip reason; empty on success

  friend bool operator==(const RunRecord&, const RunRecord&) = default;
};

// Where the scheduler keeps its state. Contract for every implementation (the SQLite one too):
// - load/save address one `schedule` row by name; save inserts or replaces the whole row.
// - Times round-trip exactly; they are whole seconds, so Unix seconds (INTEGER) suffice.
// - Writes are synchronous and durable when they return success, never batched: the scheduler
//   saves the advanced next_run before it starts a run, and that is what keeps an occurrence
//   from running twice after a crash or restart.
// - append_run adds a row to an append-only run history (`schedule_run`).
// - Rows are never deleted by retention.
// The scheduler serializes its calls, so implementations need no locking of their own.
class IScheduleRepository {
 public:
  virtual ~IScheduleRepository() = default;
  IScheduleRepository(const IScheduleRepository&) = delete;
  IScheduleRepository& operator=(const IScheduleRepository&) = delete;
  IScheduleRepository(IScheduleRepository&&) = delete;
  IScheduleRepository& operator=(IScheduleRepository&&) = delete;

  // nullopt when no row has this name.
  [[nodiscard]] virtual Result<std::optional<ScheduleState>> load(std::string_view name) = 0;
  [[nodiscard]] virtual Result<void> save(const ScheduleState& state) = 0;
  [[nodiscard]] virtual Result<void> append_run(const RunRecord& record) = 0;

 protected:
  IScheduleRepository() = default;
};

// Keeps state in memory; for tests and for running without a database.
class MemoryScheduleRepository final : public IScheduleRepository {
 public:
  MemoryScheduleRepository() = default;

  [[nodiscard]] Result<std::optional<ScheduleState>> load(std::string_view name) override;
  [[nodiscard]] Result<void> save(const ScheduleState& state) override;
  [[nodiscard]] Result<void> append_run(const RunRecord& record) override;

  [[nodiscard]] const std::map<std::string, ScheduleState, std::less<>>& rows() const {
    return rows_;
  }
  [[nodiscard]] const std::vector<RunRecord>& runs() const { return runs_; }

 private:
  std::map<std::string, ScheduleState, std::less<>> rows_;
  std::vector<RunRecord> runs_;
};

}  // namespace myclean::sched
