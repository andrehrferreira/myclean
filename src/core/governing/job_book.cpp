#include "core/governing/job_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/governing/keys.hpp"
#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governing {

namespace {

constexpr double mib = 1024.0 * 1024.0;

std::string mib_text(Bytes bytes) {
  return std::format("{:.0f} MiB", static_cast<double>(bytes.count()) / mib);
}

}  // namespace

Result<TrackedJob*> JobBook::create(governor::JobRole role, rules::ProcessClass process_class,
                                    const std::optional<governor::ProcessKey>& root,
                                    std::string root_image, const governor::JobLimits& limits,
                                    WallTime now) {
  // The object name carries the class: build, test, or the class of a capped root ("default").
  Result<std::unique_ptr<os::IJob>> created = control_->create(rules::to_string(process_class));
  if (!created) {
    return tl::unexpected(created.error());
  }
  // Filled in place: a TrackedJob is never moved (moving its sets allocates on MSVC).
  auto tracked = std::make_unique<TrackedJob>();
  tracked->job = std::move(*created);
  tracked->role = role;
  tracked->process_class = process_class;
  tracked->root = root;
  tracked->root_image = std::move(root_image);
  tracked->notification = governor::CommitNotificationLimit(rearm_percent_);
  const JobRecord record{.ref = tracked->job->ref(),
                         .role = role,
                         .process_class = process_class,
                         .root = root,
                         .root_image = tracked->root_image,
                         .created = now};
  Result<void> ready =
      set_limits(*tracked, limits).and_then([&] { return save_job(*store_, record); });
  if (!ready) {
    static_cast<void>(tracked->job->lift_all());
    static_cast<void>(tracked->job->release());
    return tl::unexpected(ready.error());
  }
  jobs_.push_back(std::move(tracked));
  return jobs_.back().get();
}

Result<TrackedJob*> JobBook::adopt(std::unique_ptr<os::IJob> job, const JobRecord& record,
                                   const governor::JobLimits& limits) {
  auto tracked = std::make_unique<TrackedJob>();
  tracked->adopted = !job->has_port();
  tracked->job = std::move(job);
  tracked->role = record.role;
  tracked->process_class = record.process_class;
  tracked->root = record.root;
  tracked->root_image = record.root_image;
  tracked->notification = governor::CommitNotificationLimit(rearm_percent_);
  if (Result<void> limited = set_limits(*tracked, limits); !limited) {
    return tl::unexpected(limited.error());
  }
  jobs_.push_back(std::move(tracked));
  return jobs_.back().get();
}

Result<void> JobBook::set_limits(TrackedJob& job, const governor::JobLimits& limits) {
  os::IJob& object = *job.job;
  Result<void> set = object.set_cpu_cap(static_cast<double>(limits.cpu_percent)).and_then([&] {
    return object.set_commit_cap(limits.commit);
  });
  if (set) {
    set = limits.notification ? object.set_commit_notification(*limits.notification)
                              : object.clear_commit_notification();
  }
  if (!set) {
    return set;
  }
  job.limits = limits;
  return {};
}

void JobBook::release(TrackedJob& job, governor::Reason reason, std::string_view detail) {
  const os::JobRef ref = job.job->ref();
  std::string outcome(detail);
  if (Result<void> lifted = job.job->lift_all(); !lifted) {
    outcome += "; lift failed: " + lifted.error().message;
  }
  if (Result<void> released = job.job->release(); !released) {
    outcome += "; release failed: " + released.error().message;
  }
  record(job, EventKind::job_released, reason, std::move(outcome));
  if (Result<void> forgotten = forget_job(*store_, ref); !forgotten) {
    spdlog::error("governor: forget job {}: {}", ref.object_name(), forgotten.error().message);
  }
  std::erase_if(jobs_,
                [&](const std::unique_ptr<TrackedJob>& tracked) { return tracked.get() == &job; });
}

void JobBook::release_all(governor::Reason reason, std::string_view detail) {
  while (!jobs_.empty()) {
    release(*jobs_.back(), reason, detail);
  }
}

void JobBook::poll(const std::map<std::uint32_t, WallTime>& live) {
  std::vector<TrackedJob*> drained;
  for (const std::unique_ptr<TrackedJob>& tracked : jobs_) {
    TrackedJob& job = *tracked;
    if (job.job->has_port()) {
      drain(job);
    } else if (!poll_members(job, live)) {
      continue;
    }
    const Result<os::JobAccounting> accounting = job.job->accounting();
    if (!accounting) {
      continue;
    }
    job.committed = accounting->committed;
    check_commit(job, accounting->committed);
    if (accounting->active_processes == 0) {
      drained.push_back(&job);
    }
  }
  for (TrackedJob* job : drained) {
    release(*job, governor::Reason::policy, "its last process exited");
  }
}

void JobBook::drain() {
  for (const std::unique_ptr<TrackedJob>& job : jobs_) {
    if (job->job->has_port()) {
      drain(*job);
    }
  }
}

void JobBook::drain(TrackedJob& job) {
  Result<std::vector<os::JobEvent>> events = job.job->poll(max_port_messages);
  if (!events) {
    spdlog::warn("governor: poll {}: {}", job.job->ref().object_name(), events.error().message);
    return;
  }
  for (const os::JobEvent& event : *events) {
    handle(job, event);
  }
}

bool JobBook::poll_members(TrackedJob& job, const std::map<std::uint32_t, WallTime>& live) {
  const Result<std::vector<std::uint32_t>> pids = job.job->members();
  if (!pids) {
    spdlog::warn("governor: members of {}: {}", job.job->ref().object_name(), pids.error().message);
    return false;
  }
  job.members.clear();
  for (const std::uint32_t pid : *pids) {
    if (const auto found = live.find(pid); found != live.end()) {
      job.members.insert(governor::ProcessKey{.pid = pid, .created = found->second});
    }
  }
  return true;
}

void JobBook::handle(TrackedJob& job, const os::JobEvent& event) {
  switch (event.kind) {
    case os::JobEventKind::new_process:
      if (event.created) {
        job.members.insert(governor::ProcessKey{.pid = event.pid, .created = *event.created});
      }
      break;
    case os::JobEventKind::exit_process:
      std::erase_if(job.members, [&](const governor::ProcessKey& member) {
        return member.pid == event.pid && (!event.created || member.created == *event.created);
      });
      job.memory_limit_pids.erase(event.pid);
      if (event.commit_failure && event.exit_code) {
        record(job, EventKind::commit_failure_exit, governor::Reason::commit,
               std::format("pid {} exit 0x{:08X}", event.pid, *event.exit_code));
      }
      break;
    case os::JobEventKind::job_memory_limit:
      if (job.memory_limit_pids.insert(event.pid).second) {
        record(job, EventKind::job_memory_limit, governor::Reason::commit,
               std::format("pid {}: a commit beyond the job's cap of {} was refused", event.pid,
                           mib_text(job.limits.commit)));
      }
      break;
    case os::JobEventKind::notification_limit:
      if (job.notification.on_limit_message() ==
          governor::CommitNotificationLimit::Action::record_and_clear) {
        const Bytes committed =
            event.violation ? event.violation->committed : job.committed.value_or(Bytes{});
        record(job, EventKind::notification_limit, governor::Reason::commit,
               std::format("job commit {} passed the notification limit {} ({} messages)",
                           mib_text(committed), mib_text(job.limits.notification.value_or(Bytes{})),
                           event.count));
        static_cast<void>(job.job->clear_commit_notification());
      }
      break;
    case os::JobEventKind::active_process_zero:
      break;
  }
}

void JobBook::check_commit(TrackedJob& job, Bytes committed) {
  if (!job.limits.notification) {
    return;
  }
  const Bytes limit = *job.limits.notification;
  if (job.notification.armed() && committed > limit &&
      job.notification.on_limit_message() ==
          governor::CommitNotificationLimit::Action::record_and_clear) {
    record(job, EventKind::notification_limit, governor::Reason::commit,
           std::format("job commit {} passed the notification limit {}", mib_text(committed),
                       mib_text(limit)));
    static_cast<void>(job.job->clear_commit_notification());
    return;
  }
  if (job.notification.on_sample(committed, job.limits.commit) ==
      governor::CommitNotificationLimit::Action::rearm) {
    if (Result<void> armed = job.job->set_commit_notification(limit); !armed) {
      spdlog::warn("governor: re-arm {}: {}", job.job->ref().object_name(), armed.error().message);
    }
  }
}

void JobBook::record(const TrackedJob& job, EventKind kind, governor::Reason reason,
                     std::string detail) {
  recorder_->record(
      GovernorEvent{.kind = kind,
                    .root = job.root,
                    .root_image = job.root_image,
                    .process_class = job.process_class,
                    .reason = reason,
                    .members = job.members.size(),
                    .detail = std::format("{} job {}: {}", governor::to_string(job.role),
                                          job.job->ref().object_name(), detail)},
      false);
}

Result<governor::JobMembership> JobBook::membership(const governor::ProcessKey& process) {
  const Result<bool> in_job = control_->in_any_job(to_os(process));
  if (!in_job) {
    return tl::unexpected(in_job.error());
  }
  if (!*in_job) {
    return governor::JobMembership::none;
  }
  for (const std::unique_ptr<TrackedJob>& job : jobs_) {
    if (job->members.contains(process)) {
      return governor::JobMembership::myclean;
    }
    if (const Result<bool> contains = job->job->contains(to_os(process)); contains && *contains) {
      return governor::JobMembership::myclean;
    }
  }
  return governor::JobMembership::foreign;
}

bool JobBook::in_tree_job(const governor::ProcessKey& process) const {
  return std::ranges::any_of(jobs_, [&](const std::unique_ptr<TrackedJob>& job) {
    return job->role != governor::JobRole::cap_job && job->members.contains(process);
  });
}

TrackedJob* JobBook::class_job(rules::ProcessClass process_class) {
  const auto found = std::ranges::find_if(jobs_, [&](const std::unique_ptr<TrackedJob>& job) {
    return job->role == governor::JobRole::class_job && job->process_class == process_class;
  });
  return found == jobs_.end() ? nullptr : found->get();
}

TrackedJob* JobBook::tree_job_of(const governor::ProcessKey& process) {
  const auto found = std::ranges::find_if(jobs_, [&](const std::unique_ptr<TrackedJob>& job) {
    return job->role == governor::JobRole::tree_job &&
           (job->members.contains(process) || job->root == process);
  });
  return found == jobs_.end() ? nullptr : found->get();
}

std::vector<TrackedJob*> JobBook::caps_of(const std::vector<governor::ProcessKey>& processes) {
  std::vector<TrackedJob*> caps;
  for (const std::unique_ptr<TrackedJob>& job : jobs_) {
    const bool cap = job->role == governor::JobRole::cap_job || job->tightened_from.has_value();
    if (cap && std::ranges::any_of(processes, [&](const governor::ProcessKey& process) {
          return job->members.contains(process) || job->root == process;
        })) {
      caps.push_back(job.get());
    }
  }
  return caps;
}

}  // namespace myclean::governing
