#include "core/sched/store_schedule_repository.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/sched/schedule_repository.hpp"
#include "core/store/database.hpp"
#include "core/store/schedule_repository.hpp"

namespace myclean::sched {

Result<std::optional<ScheduleState>> StoreScheduleRepository::load(std::string_view name) {
  std::optional<store::ScheduledJob> job;
  const Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
    Result<std::optional<store::ScheduledJob>> loaded = store::load_job(db, name);
    if (!loaded) {
      return tl::unexpected(loaded.error());
    }
    job = *std::move(loaded);
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  if (!job) {
    return std::optional<ScheduleState>{};
  }
  std::optional<RunStatus> last_result;
  if (job->last_result) {
    last_result = parse_run_status(*job->last_result);
    if (!last_result) {
      return fail(Errc::invalid_data, "schedule row '" + job->name + "' has an unknown result '" +
                                          *job->last_result + "'");
    }
  }
  return ScheduleState{.name = job->name,
                       .expression = job->expression,
                       .last_run = job->last_run,
                       .last_result = last_result,
                       .next_run = job->next_run};
}

Result<void> StoreScheduleRepository::save(const ScheduleState& state) {
  const store::ScheduledJob job{
      .name = state.name,
      .expression = state.expression,
      .last_run = state.last_run,
      .last_result = state.last_result
                         ? std::optional<std::string>(std::string(to_string(*state.last_result)))
                         : std::nullopt,
      .next_run = state.next_run};
  return store_->write([&](store::Database& db) { return store::save_job(db, job); });
}

Result<void> StoreScheduleRepository::append_run(const RunRecord& record) {
  const store::JobRun run{.job = record.job,
                          .started = record.started,
                          .catch_up = record.catch_up,
                          .status = std::string(to_string(record.status)),
                          .detail = record.detail};
  return store_->write([&](store::Database& db) { return store::add_job_run(db, run); });
}

}  // namespace myclean::sched
