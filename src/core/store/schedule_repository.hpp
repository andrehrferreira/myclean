#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::store {

// Rows of the scheduler's tables, in the shape of core/sched's ScheduleState and RunRecord;
// statuses are their text forms. Times are stored to the second. Writes are synchronous: call
// these inside Store::write.

// One row of `schedule`.
struct ScheduledJob {
  std::string name;
  std::string expression;  // five-field cron expression
  std::optional<WallTime> last_run;
  std::optional<std::string> last_result;
  WallTime next_run;

  friend bool operator==(const ScheduledJob&, const ScheduledJob&) = default;
};

// One row of `schedule_run`: a run, a catch-up run or a skipped occurrence.
struct JobRun {
  std::string job;
  WallTime started;
  bool catch_up = false;
  std::string status;  // "succeeded", "failed", "skipped", ...
  std::string detail;  // the failure or skip reason
};

// Inserts the job, or replaces the whole row of the job with the same name.
[[nodiscard]] Result<void> save_job(Database& db, const ScheduledJob& job);
// nullopt when no job has `name`.
[[nodiscard]] Result<std::optional<ScheduledJob>> load_job(Database& db, std::string_view name);

// Appends to the run history.
[[nodiscard]] Result<void> add_job_run(Database& db, const JobRun& run);
// The runs of `job`, oldest first.
[[nodiscard]] Result<std::vector<JobRun>> job_runs(Database& db, std::string_view job);

}  // namespace myclean::store
