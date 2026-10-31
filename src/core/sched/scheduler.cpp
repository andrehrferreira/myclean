#include "core/sched/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/sched/cron.hpp"
#include "core/sched/schedule_repository.hpp"

namespace myclean::sched {
namespace {

[[nodiscard]] WallTime whole_second(WallTime time) {
  return std::chrono::floor<std::chrono::seconds>(time);
}

}  // namespace

Scheduler::Scheduler(const os::IClock& clock, IScheduleRepository& repository)
    : clock_(&clock), repository_(&repository) {}

Result<void> Scheduler::add(std::string name, std::string_view expression) {
  auto cron = CronExpression::parse(expression);
  if (!cron) {
    return tl::unexpected(cron.error());
  }
  const std::scoped_lock lock(mutex_);
  if (jobs_.contains(name)) {
    return fail(Errc::already_exists, std::format("job '{}' is already registered", name));
  }
  auto loaded = repository_->load(name);
  if (!loaded) {
    return tl::unexpected(loaded.error());
  }
  const std::optional<ScheduleState>& stored = *loaded;

  ScheduleState state;
  if (stored && stored->expression == cron->text()) {
    state = *stored;
  } else {
    const auto next = cron->next_after(clock_->now(), clock_->zone());
    if (!next) {
      return fail(Errc::invalid_argument,
                  std::format("'{}' never matches a calendar date", expression));
    }
    state =
        ScheduleState{.name = name, .expression = cron->text(), .next_run = whole_second(*next)};
    if (stored) {
      state.last_run = stored->last_run;
      state.last_result = stored->last_result;
    }
  }

  const bool interrupted = state.last_result == RunStatus::running && state.last_run.has_value();
  if (interrupted) {
    state.last_result = RunStatus::failed;
  }
  if (state != stored) {
    if (auto saved = repository_->save(state); !saved) {
      return saved;
    }
  }
  if (interrupted) {
    auto recorded =
        repository_->append_run({.job = name,
                                 .started = *state.last_run,
                                 .status = RunStatus::failed,
                                 .detail = "interrupted: MyClean stopped before the run finished"});
    if (!recorded) {
      return recorded;
    }
  }
  jobs_.emplace(std::move(name), Job{.cron = std::move(*cron), .state = std::move(state)});
  return {};
}

PollResult Scheduler::poll() {
  const std::scoped_lock lock(mutex_);
  const WallTime now = clock_->now();
  PollResult result;
  for (auto& [name, job] : jobs_) {
    if (job.state.next_run > now) {
      continue;
    }
    const auto next = job.cron.next_after(now, clock_->zone());
    if (!next) {
      result.errors.push_back(
          {.code = Errc::invalid_state,
           .message = std::format("job '{}': '{}' has no next run", name, job.state.expression)});
      continue;
    }
    const bool catch_up = now - job.state.next_run > catch_up_grace;
    ScheduleState state = job.state;
    state.next_run = whole_second(*next);
    if (!job.active) {
      state.last_run = whole_second(now);
      state.last_result = RunStatus::running;
    }
    if (auto saved = repository_->save(state); !saved) {
      result.errors.push_back(saved.error());
      continue;
    }
    job.state = std::move(state);

    if (job.active) {
      auto recorded =
          repository_->append_run({.job = name,
                                   .started = whole_second(now),
                                   .catch_up = catch_up,
                                   .status = RunStatus::skipped,
                                   .detail = "skipped: the previous run is still active"});
      if (!recorded) {
        result.errors.push_back(recorded.error());
      }
      continue;
    }
    job.active = true;
    job.catch_up = catch_up;
    result.runs.push_back({.job = name, .catch_up = catch_up});
  }
  return result;
}

Result<void> Scheduler::finish(std::string_view job_name, const Result<void>& outcome) {
  const std::scoped_lock lock(mutex_);
  const auto found = jobs_.find(job_name);
  if (found == jobs_.end()) {
    return fail(Errc::not_found, std::format("no job named '{}'", job_name));
  }
  Job& job = found->second;
  if (!job.active || !job.state.last_run) {
    return fail(Errc::invalid_state, std::format("job '{}' has no active run", job_name));
  }
  job.active = false;
  job.state.last_result = outcome ? RunStatus::succeeded : RunStatus::failed;
  auto saved = repository_->save(job.state);
  auto recorded = repository_->append_run({.job = job.state.name,
                                           .started = *job.state.last_run,
                                           .catch_up = job.catch_up,
                                           .status = *job.state.last_result,
                                           .detail = outcome ? "" : outcome.error().message});
  return saved ? recorded : saved;
}

std::optional<WallTime> Scheduler::next_wake() const {
  const std::scoped_lock lock(mutex_);
  std::optional<WallTime> earliest;
  for (const auto& entry : jobs_) {
    const WallTime next = entry.second.state.next_run;
    earliest = earliest ? std::min(*earliest, next) : next;
  }
  return earliest;
}

}  // namespace myclean::sched
