#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/sched/cron.hpp"
#include "core/sched/schedule_repository.hpp"

namespace myclean::sched {

// A job run the caller must start now, then report with Scheduler::finish().
struct DueRun {
  std::string job;
  bool catch_up = false;

  friend bool operator==(const DueRun&, const DueRun&) = default;
};

struct PollResult {
  std::vector<DueRun> runs;
  // Storage failures. A job whose state could not be saved is not started; it stays due and the
  // next poll retries it.
  std::vector<Error> errors;
};

// MyClean's own scheduler; it never touches an operating-system schedule. It owns no thread:
// the service loop calls poll() when next_wake() arrives and at least every few seconds (sleeping
// until next_wake() alone misses wall-clock changes and wake-ups from sleep), starts the returned
// runs, and calls finish() when each ends. Every method is thread-safe.
//
// Guarantees:
// - Missed occurrences (MyClean off, machine asleep, clock moved forward) coalesce into one
//   catch-up run; the next run is then computed from the current time.
// - A job never runs twice at once: an occurrence that comes due while its previous run is still
//   active is recorded as skipped.
// - An occurrence runs at most once: next_run is advanced and saved before a run starts, so
//   neither a restart nor a clock moved back (manually or at the end of daylight saving) repeats
//   it. The price: a clock moved back delays the next run by the same amount.
class Scheduler {
 public:
  // A run that starts more than this after its occurrence is marked as a catch-up.
  static constexpr std::chrono::minutes catch_up_grace{1};

  // `clock` and `repository` must outlive the scheduler.
  Scheduler(const os::IClock& clock, IScheduleRepository& repository);

  // Registers a job and restores its persisted state. When the stored expression is the same,
  // last and next run are kept as stored (a passed next_run is caught up by the next poll);
  // otherwise next_run is computed from now. A run that was still active when MyClean stopped is
  // recorded as failed. Fails, storing nothing, on an invalid or never-matching expression or a
  // name already registered.
  [[nodiscard]] Result<void> add(std::string name, std::string_view expression);

  // Starts every job whose next run has come: returns the runs to start and saves each job's
  // next run, computed from now.
  [[nodiscard]] PollResult poll();

  // Ends the active run of `job` with its outcome and records it.
  [[nodiscard]] Result<void> finish(std::string_view job, const Result<void>& outcome);

  // The earliest next run of any job; nullopt when no job is registered.
  [[nodiscard]] std::optional<WallTime> next_wake() const;

 private:
  struct Job {
    CronExpression cron;
    ScheduleState state;
    bool active = false;
    bool catch_up = false;  // of the active run
  };

  const os::IClock* clock_;
  IScheduleRepository* repository_;
  mutable std::mutex mutex_;
  std::map<std::string, Job, std::less<>> jobs_;
};

}  // namespace myclean::sched
