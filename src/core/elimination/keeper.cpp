#include "core/elimination/keeper.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/actions/audit_log.hpp"
#include "core/alerts/engine.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::elimination {
namespace {

std::string rate_metrics(const Rate& rate) {
  return std::format("incidents={} hours={:.2f}", rate.incidents, rate.hours);
}

}  // namespace

std::vector<os::ProcessKey> PauseKeeper::suspendable(std::string_view image) const {
  std::vector<os::ProcessKey> found;
  const std::shared_ptr<const telemetry::ProcessView> view =
      sources_.view ? sources_.view() : nullptr;
  if (view == nullptr || !sources_.protects) {
    return found;
  }
  for (const telemetry::ProcessRow& row : view->rows) {
    const telemetry::ProcessInfo& info = row.info;
    if (!same_image(info.image, image) || !info.session || *info.session == 0U) {
      continue;  // another program, or a service (session 0) or a session not reported
    }
    const os::ProcessKey key{.pid = info.pid, .created = info.created};
    if (!sources_.protects(key)) {
      found.push_back(key);
    }
  }
  return found;
}

std::optional<bool> PauseKeeper::running(std::string_view image) const {
  const std::shared_ptr<const telemetry::ProcessView> view =
      sources_.view ? sources_.view() : nullptr;
  if (view == nullptr) {
    return std::nullopt;
  }
  return std::ranges::any_of(view->rows, [&](const telemetry::ProcessRow& row) {
    return same_image(row.info.image, image);
  });
}

Result<void> PauseKeeper::store_pause(PauseRecord& record,
                                      std::span<const os::ProcessKey> processes,
                                      std::span<const WallTime> times) {
  return store_->write([&](store::Database& db) -> Result<void> {
    Result<std::optional<PauseRecord>> other = active_pause(db);
    if (!other) {
      return tl::unexpected(other.error());
    }
    if (*other) {
      return fail(Errc::invalid_state, "a pause of " + (*other)->image + " is active");
    }
    Result<std::vector<Span>> periods = pause_periods(db);
    if (!periods) {
      return tl::unexpected(periods.error());
    }
    const std::optional<Rate> rate = baseline(times, record.started, *periods);
    if (!rate) {
      return fail(Errc::invalid_state, "a baseline of freeze incidents is still being collected");
    }
    record.baseline = *rate;
    Result<std::int64_t> id = begin_pause(db, record, processes);
    if (!id) {
      return tl::unexpected(id.error());
    }
    record.id = *id;
    const Result<std::int64_t> logged = actions::append_log(
        db, actions::LogEntry{.time = record.started,
                              .plan = record.plan,
                              .actor = Actor::owner().to_string(),
                              .event = "pause",
                              .outcome = std::string(to_string(record.mode)),
                              .detail = record.image,
                              .metrics = "processes=" + std::to_string(processes.size()) + " " +
                                         rate_metrics(record.baseline)});
    return logged ? Result<void>{} : tl::unexpected(logged.error());
  });
}

Result<PauseRecord> PauseKeeper::pause(PlanId plan, std::string_view image, PauseMode mode,
                                       std::chrono::hours period) {
  const std::scoped_lock lock(mutex_);
  const WallTime now = clock_->now();
  Result<std::vector<WallTime>> times = input::incident_times(*store_);
  if (!times) {
    return tl::unexpected(times.error());
  }
  std::vector<os::ProcessKey> processes;
  if (mode == PauseMode::suspend) {
    if (sources_.processes == nullptr) {
      return fail(Errc::invalid_state, "no process control to suspend " + std::string(image));
    }
    processes = suspendable(image);
    if (processes.empty()) {
      return fail(Errc::invalid_state, "no process of " + std::string(image) +
                                           " in the owner's session may be suspended now");
    }
  }
  PauseRecord record{.plan = plan,
                     .image = std::string(image),
                     .mode = mode,
                     .started = now,
                     .ends = now + period};
  // The pause and its processes are stored before anything is suspended.
  if (Result<void> stored = store_pause(record, processes, *times); !stored) {
    return tl::unexpected(stored.error());
  }
  std::size_t suspended = 0;
  for (const os::ProcessKey& process : processes) {
    if (sources_.processes->suspend(process)) {
      ++suspended;
      continue;
    }
    static_cast<void>(
        store_->write([&](store::Database& db) { return forget_process(db, record.id, process); }));
  }
  if (mode == PauseMode::suspend && suspended == 0) {
    if (Result<void> ended = finish(record, EndReason::failed); !ended) {
      return tl::unexpected(ended.error());
    }
    return fail(Errc::invalid_state, "no process of " + record.image + " could be suspended");
  }
  open_span_.reset();
  span_saved_.reset();
  return record;
}

std::optional<Error> PauseKeeper::resume_recorded(const PauseRecord& pause) {
  Result<std::vector<PausedProcess>> processes = fail(Errc::io_error, "not read");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        processes = paused_processes(db, pause.id);
        return {};
      });
      !read) {
    return read.error();
  }
  if (!processes) {
    return processes.error();
  }
  std::optional<Error> failure;
  for (const PausedProcess& paused : *processes) {
    if (std::optional<Error> failed = resume(paused)) {
      failure = std::move(failed);  // it stays recorded: the next tick tries again
      continue;
    }
    if (Result<void> forgot = store_->write(
            [&](store::Database& db) { return forget_process(db, pause.id, paused.process); });
        !forgot) {
      return forgot.error();
    }
  }
  return failure;
}

std::optional<Error> PauseKeeper::resume(const PausedProcess& paused) const {
  if (sources_.processes == nullptr) {
    return Error{.code = Errc::invalid_state, .message = "no process control"};
  }
  for (int time = 0; time < paused.count; ++time) {
    const Result<void> resumed = sources_.processes->resume(paused.process);
    if (resumed) {
      continue;
    }
    if (resumed.error().code == Errc::not_found) {
      return std::nullopt;  // gone (or its PID reused): nothing left to resume
    }
    return resumed.error();
  }
  return std::nullopt;
}

Result<void> PauseKeeper::close(const PauseRecord& pause, EndReason reason) {
  const WallTime now = clock_->now();
  Result<std::vector<WallTime>> times = input::incident_times(*store_);
  if (!times) {
    return tl::unexpected(times.error());
  }
  return store_->write([&](store::Database& db) -> Result<void> {
    if (pause.mode == PauseMode::suspend) {
      if (Result<void> span = save_span(db, pause.id, pause.started, now); !span) {
        return span;
      }
    }
    Result<std::vector<Span>> spans = spans_of(db, pause.id);
    if (!spans) {
      return tl::unexpected(spans.error());
    }
    const Rate during = reason == EndReason::failed ? Rate{} : rate_in(*times, *spans);
    if (Result<void> finished = finish_pause(db, pause.id, now, reason, during); !finished) {
      return finished;
    }
    const Result<std::int64_t> logged = actions::append_log(
        db,
        actions::LogEntry{.time = now,
                          .plan = pause.plan,
                          .actor = Actor::owner().to_string(),
                          .event = "restore",
                          .outcome = std::string(to_string(reason)),
                          .detail = pause.image + " (" + std::string(to_string(pause.mode)) + ")",
                          .metrics = rate_metrics(during)});
    return logged ? Result<void>{} : tl::unexpected(logged.error());
  });
}

Result<void> PauseKeeper::finish(const PauseRecord& pause, EndReason reason) {
  if (pause.mode == PauseMode::suspend) {
    if (std::optional<Error> failure = resume_recorded(pause)) {
      return tl::unexpected(*std::move(failure));  // the pause stays active
    }
  } else if (Result<void> tracked = track(pause, true); !tracked) {
    return tracked;
  }
  if (Result<void> closed = close(pause, reason); !closed) {
    return closed;
  }
  open_span_.reset();
  span_saved_.reset();
  if (pause.mode == PauseMode::guided && sources_.alerts != nullptr) {
    const std::vector<alerts::Reading> reminder = {alerts::Reading{
        .metric = std::string(guided_end_metric),
        .entity = std::to_string(pause.plan.value()),
        .value = 1.0,
        .detail = "The pause of " + pause.image + " is over: start it again when you want it"}};
    sources_.alerts->report(reminder);
  }
  return {};
}

Result<void> PauseKeeper::track(const PauseRecord& pause, bool flush) {
  const WallTime now = clock_->now();
  const std::optional<bool> runs = running(pause.image);
  if (!runs) {
    return {};  // no process table yet: nothing can be judged
  }
  if (*runs) {
    if (open_span_) {
      const WallTime from = *open_span_;
      open_span_.reset();
      return store_->write([&](store::Database& db) { return save_span(db, pause.id, from, now); });
    }
    return {};
  }
  if (!open_span_) {
    open_span_ = now;
  }
  const MonoTime mono = clock_->monotonic();
  if (!flush && span_saved_ && mono - *span_saved_ < span_flush) {
    return {};
  }
  span_saved_ = mono;
  const WallTime from = open_span_.value_or(now);
  return store_->write([&](store::Database& db) { return save_span(db, pause.id, from, now); });
}

Result<void> PauseKeeper::end(PlanId plan, EndReason reason) {
  const std::scoped_lock lock(mutex_);
  Result<std::optional<PauseRecord>> found = fail(Errc::io_error, "not read");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        found = pause_of_plan(db, plan);
        return {};
      });
      !read) {
    return read;
  }
  if (!found) {
    return tl::unexpected(found.error());
  }
  const std::optional<PauseRecord>& pause = *found;
  if (!pause.has_value()) {
    return fail(Errc::not_found, "plan " + std::to_string(plan.value()) + " has no pause");
  }
  return pause->active() ? finish(*pause, reason) : Result<void>{};
}

Result<std::optional<PauseRecord>> PauseKeeper::record(PlanId plan) const {
  Result<std::optional<PauseRecord>> found = fail(Errc::io_error, "not read");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        found = pause_of_plan(db, plan);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  return found;
}

Result<std::optional<PauseRecord>> PauseKeeper::active() const {
  Result<std::optional<PauseRecord>> found = fail(Errc::io_error, "not read");
  if (Result<void> read = store_->read([&](store::Database& db) -> Result<void> {
        found = active_pause(db);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  return found;
}

void PauseKeeper::settle(const std::function<Result<void>(const PauseRecord&)>& step) {
  const Result<std::optional<PauseRecord>> pause = active();
  Result<void> outcome;
  if (!pause) {
    outcome = tl::unexpected(pause.error());
  } else if (const std::optional<PauseRecord>& current = *pause; current.has_value()) {
    outcome = step(*current);
  }
  problem_ = outcome ? std::string() : outcome.error().message;
}

void PauseKeeper::tick() {
  const std::scoped_lock lock(mutex_);
  settle([&](const PauseRecord& pause) -> Result<void> {
    if (clock_->now() >= pause.ends) {
      return finish(pause, EndReason::period_ended);
    }
    return pause.mode == PauseMode::guided ? track(pause) : Result<void>{};
  });
}

void PauseKeeper::recover() {
  const std::scoped_lock lock(mutex_);
  settle([&](const PauseRecord& pause) -> Result<void> {
    if (clock_->now() >= pause.ends) {
      return finish(pause, EndReason::period_ended);
    }
    return pause.mode == PauseMode::suspend ? finish(pause, EndReason::service_restarted)
                                            : Result<void>{};
  });
}

void PauseKeeper::stop() {
  const std::scoped_lock lock(mutex_);
  settle([&](const PauseRecord& pause) -> Result<void> {
    // A guided pause goes on: its span so far is kept.
    return pause.mode == PauseMode::suspend ? finish(pause, EndReason::service_stopped)
                                            : track(pause, true);
  });
}

std::string PauseKeeper::problem() const {
  const std::scoped_lock lock(mutex_);
  return problem_;
}

}  // namespace myclean::elimination
