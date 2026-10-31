#include "core/store/schedule_repository.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"

namespace myclean::store {

Result<void> save_job(Database& db, const ScheduledJob& job) {
  Result<Statement> upsert = db.prepare(
      "INSERT INTO schedule(job, expression, last_run, last_result, next_run) "
      "VALUES(?1, ?2, ?3, ?4, ?5) ON CONFLICT(job) DO UPDATE SET "
      "expression = excluded.expression, last_run = excluded.last_run, "
      "last_result = excluded.last_result, next_run = excluded.next_run");
  if (!upsert) {
    return tl::unexpected(upsert.error());
  }
  upsert->bind(1, job.name).bind(2, job.expression).bind(5, to_unix_seconds(job.next_run));
  if (job.last_run) {
    upsert->bind(3, to_unix_seconds(*job.last_run));
  } else {
    upsert->bind(3, nullptr);
  }
  if (job.last_result) {
    upsert->bind(4, *job.last_result);
  } else {
    upsert->bind(4, nullptr);
  }
  return upsert->run();
}

Result<std::optional<ScheduledJob>> load_job(Database& db, std::string_view name) {
  Result<Statement> query =
      db.prepare("SELECT expression, last_run, last_result, next_run FROM schedule WHERE job = ?1");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, name);
  const Result<bool> row = query->step();
  if (!row) {
    return tl::unexpected(row.error());
  }
  if (!*row) {
    return std::nullopt;
  }
  return ScheduledJob{
      .name = std::string(name),
      .expression = query->column_text(0),
      .last_run =
          query->is_null(1) ? std::nullopt : std::optional(from_unix_seconds(query->column_int(1))),
      .last_result = query->is_null(2) ? std::nullopt : std::optional(query->column_text(2)),
      .next_run = from_unix_seconds(query->column_int(3))};
}

Result<void> add_job_run(Database& db, const JobRun& run) {
  Result<Statement> insert = db.prepare(
      "INSERT INTO schedule_run(job, started_at, catch_up, status, detail) "
      "VALUES(?1, ?2, ?3, ?4, ?5)");
  if (!insert) {
    return tl::unexpected(insert.error());
  }
  return insert->bind(1, run.job)
      .bind(2, to_unix_seconds(run.started))
      .bind(3, run.catch_up)
      .bind(4, run.status)
      .bind(5, run.detail)
      .run();
}

Result<std::vector<JobRun>> job_runs(Database& db, std::string_view job) {
  Result<Statement> query = db.prepare(
      "SELECT started_at, catch_up, status, detail FROM schedule_run "
      "WHERE job = ?1 ORDER BY started_at, id");
  if (!query) {
    return tl::unexpected(query.error());
  }
  query->bind(1, job);
  std::vector<JobRun> runs;
  Result<bool> row = query->step();
  for (; row && *row; row = query->step()) {
    runs.push_back(JobRun{.job = std::string(job),
                          .started = from_unix_seconds(query->column_int(0)),
                          .catch_up = query->column_int(1) != 0,
                          .status = query->column_text(2),
                          .detail = query->column_text(3)});
  }
  if (!row) {
    return tl::unexpected(row.error());
  }
  return runs;
}

}  // namespace myclean::store
