#include "core/governing/executor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "core/governing/job_book.hpp"
#include "core/governing/keys.hpp"
#include "core/governing/records.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/result.hpp"
#include "core/os/process_control.hpp"

namespace myclean::governing {

namespace {

// The outcome over the members of one command.
class Tally {
 public:
  explicit Tally(std::size_t total) : total_(total) {}

  // A process that exited meanwhile (not_found) counts as gone, not as a failure.
  void add(std::uint32_t pid, const Result<void>& result) {
    if (result) {
      ++done_;
    } else if (result.error().code == Errc::not_found) {
      ++gone_;
    } else {
      fail(pid, result.error().message);
    }
  }
  void fail(std::uint32_t pid, std::string_view message) {
    if (failed_++ == 0) {
      first_error_ = std::format("pid {}: {}", pid, message);
    }
  }

  // "3 of 4 processes, 1 exited, 1 failed (pid 12: access denied)"
  [[nodiscard]] std::string text() const {
    std::string out = std::format("{} of {} processes", done_, total_);
    if (gone_ > 0) {
      out += std::format(", {} exited", gone_);
    }
    if (failed_ > 0) {
      out += std::format(", {} failed ({})", failed_, first_error_);
    }
    return out;
  }

 private:
  std::size_t total_;
  std::size_t done_ = 0;
  std::size_t gone_ = 0;
  std::size_t failed_ = 0;
  std::string first_error_;
};

template <typename T>
Result<void> as_void(const Result<T>& result) {
  if (!result) {
    return tl::unexpected(result.error());
  }
  return {};
}

// The first failure of two results.
Result<void> both(const Result<void>& first, const Result<void>& second) {
  return first ? second : first;
}

// The command's members and its root, which identify the jobs of its tree.
std::vector<governor::ProcessKey> tree_keys(const governor::Command& command) {
  std::vector<governor::ProcessKey> keys = command.members;
  keys.push_back(command.root);
  return keys;
}

}  // namespace

void Executor::execute(const governor::Command& command) {
  if (mode_ == governor::GovernorMode::observe) {
    if (command.kind != governor::CommandKind::persist_suspend) {
      record(command, "observe");
    }
    return;
  }
  switch (command.kind) {
    case governor::CommandKind::apply_step:
      record(command, apply(command));
      break;
    case governor::CommandKind::roll_back:
      record(command, roll_back(command));
      break;
    case governor::CommandKind::persist_suspend: {
      const std::string outcome = persist(command);
      if (!outcome.empty()) {
        spdlog::error("governor: {}", outcome);
      }
      break;
    }
    case governor::CommandKind::resume:
      record(command, resume(command));
      break;
    case governor::CommandKind::kill:
      record(command, kill(command));
      break;
    case governor::CommandKind::notify:
    case governor::CommandKind::add_to_allow_list:
      break;
  }
}

std::string Executor::apply(const governor::Command& command) {
  Tally tally(command.members.size());
  switch (command.step) {
    case governor::Step::priority:
      for (const governor::ProcessKey& key : command.members) {
        const Result<os::PriorityClass> priority =
            control_->set_priority(to_os(key), os::PriorityClass::below_normal);
        const Result<os::MemoryPriority> memory =
            control_->set_memory_priority(to_os(key), os::MemoryPriority::low);
        Saved& saved = saved_[key];
        if (priority && !saved.priority) {
          saved.priority = *priority;
        }
        if (memory && !saved.memory) {
          saved.memory = *memory;
        }
        tally.add(key.pid, both(as_void(priority), as_void(memory)));
      }
      return tally.text();
    case governor::Step::ecoqos:
      for (const governor::ProcessKey& key : command.members) {
        const Result<os::EcoQos> eco_qos = control_->set_eco_qos(to_os(key), os::EcoQos::on);
        Saved& saved = saved_[key];
        if (eco_qos && !saved.eco_qos) {
          saved.eco_qos = *eco_qos;
        }
        tally.add(key.pid, as_void(eco_qos));
      }
      return tally.text();
    case governor::Step::cap_job:
      return apply_cap(command);
    case governor::Step::suspend:
      return suspend(command);
    case governor::Step::kill:
      return kill(command);
  }
  return tally.text();
}

std::string Executor::apply_cap(const governor::Command& command) {
  if (!command.cap) {
    return "no cap limits";
  }
  const governor::JobLimits& cap = *command.cap;
  if (command.reason == governor::Reason::joined_tree) {
    // Children inherit the cap job; a process that joined the tree otherwise is assigned.
    Tally tally(command.members.size());
    for (TrackedJob* job : jobs_->caps_of(tree_keys(command))) {
      if (job->role != governor::JobRole::cap_job) {
        continue;
      }
      for (const governor::ProcessKey& key : command.members) {
        tally.add(key.pid, job->job->assign(to_os(key)));
      }
    }
    return tally.text();
  }
  if (command.in_myclean_job) {
    if (TrackedJob* tree = jobs_->tree_job_of(command.root); tree != nullptr) {
      return tighten(*tree, cap);
    }
  }
  Result<TrackedJob*> created = jobs_->create(governor::JobRole::cap_job, command.process_class,
                                              command.root, command.root_image, cap, clock_->now());
  if (!created) {
    return "cap job not created: " + created.error().message;
  }
  TrackedJob& job = **created;
  Tally tally(command.members.size());
  // The root first: the empty cap job nests under the job the root is in, if any.
  std::vector<governor::ProcessKey> order;
  order.reserve(command.members.size());
  if (std::ranges::find(command.members, command.root) != command.members.end()) {
    order.push_back(command.root);
  }
  std::ranges::copy_if(command.members, std::back_inserter(order),
                       [&](const governor::ProcessKey& key) { return key != command.root; });
  for (const governor::ProcessKey& key : order) {
    const Result<void> assigned = job.job->assign(to_os(key));
    if (assigned) {
      job.members.insert(key);
    }
    tally.add(key.pid, assigned);
  }
  return std::format("cap job {} (CPU {} %, commit {} MiB): {}", job.job->ref().object_name(),
                     cap.cpu_percent, cap.commit.count() >> 20U, tally.text());
}

std::string Executor::tighten(TrackedJob& job, const governor::JobLimits& cap) {
  const governor::JobLimits base = job.tightened_from.value_or(job.limits);
  job.tightened_from = base;
  governor::JobLimits tightened{.cpu_percent = std::min(base.cpu_percent, cap.cpu_percent),
                                .commit = std::min(base.commit, cap.commit)};
  if (base.notification && *base.notification < tightened.commit) {
    tightened.notification = base.notification;
  }
  const Result<void> set = JobBook::set_limits(job, tightened);
  const std::string text =
      std::format("tree job {} tightened to CPU {} %, commit {} MiB", job.job->ref().object_name(),
                  tightened.cpu_percent, tightened.commit.count() >> 20U);
  return set ? text : text + " failed: " + set.error().message;
}

std::string Executor::roll_back(const governor::Command& command) {
  Tally tally(command.members.size());
  for (const governor::ProcessKey& key : command.members) {
    const auto found = saved_.find(key);
    if (found == saved_.end()) {
      continue;
    }
    const Saved saved = found->second;
    saved_.erase(found);
    Result<void> restored;
    if (saved.priority) {
      restored = both(restored, as_void(control_->set_priority(to_os(key), *saved.priority)));
    }
    if (saved.memory) {
      restored = both(restored, as_void(control_->set_memory_priority(to_os(key), *saved.memory)));
    }
    if (saved.eco_qos) {
      restored = both(restored, as_void(control_->set_eco_qos(to_os(key), *saved.eco_qos)));
    }
    tally.add(key.pid, restored);
  }
  std::string text = tally.text();
  if (command.step == governor::Step::cap_job) {
    text += lift_caps(command);
  }
  return text;
}

std::string Executor::lift_caps(const governor::Command& command) {
  std::string text;
  for (TrackedJob* job : jobs_->caps_of(tree_keys(command))) {
    if (job->role == governor::JobRole::cap_job) {
      text += "; cap job " + job->job->ref().object_name() + " lifted";
      jobs_->release(*job, command.reason, "cap step rolled back");
      continue;
    }
    if (!job->tightened_from) {
      continue;
    }
    const governor::JobLimits before = *job->tightened_from;
    job->tightened_from.reset();
    const Result<void> restored = JobBook::set_limits(*job, before);
    text += "; tree job " + job->job->ref().object_name() +
            (restored ? " limits restored" : " restore failed: " + restored.error().message);
  }
  return text;
}

std::string Executor::persist(const governor::Command& command) {
  persisted_.clear();
  if (Result<void> written = add_suspends(*store_, command.members); !written) {
    return "suspend not persisted, so not applied: " + written.error().message;
  }
  persisted_.insert(command.members.begin(), command.members.end());
  return {};
}

std::string Executor::suspend(const governor::Command& command) {
  Tally tally(command.members.size());
  for (const governor::ProcessKey& key : command.members) {
    if (!persisted_.contains(key)) {
      tally.fail(key.pid, "its suspend record was not written");
      continue;
    }
    const Result<void> suspended = control_->suspend(to_os(key));
    if (suspended) {
      ++suspended_[key];
    } else if (Result<void> taken = remove_suspends(*store_, key, 1); !taken) {
      spdlog::error("governor: suspend record of pid {}: {}", key.pid, taken.error().message);
    }
    tally.add(key.pid, suspended);
  }
  persisted_.clear();
  return tally.text();
}

std::string Executor::resume(const governor::Command& command) {
  Tally tally(command.members.size());
  for (const governor::ProcessKey& key : command.members) {
    const auto found = suspended_.find(key);
    if (found == suspended_.end() || found->second <= 0) {
      continue;  // never suspended by this executor: never resumed by it
    }
    const Result<void> resumed = control_->resume(to_os(key));
    tally.add(key.pid, resumed);
    if (!resumed && resumed.error().code != Errc::not_found) {
      continue;  // still suspended: keep the record for the stop or the next start
    }
    if (--found->second == 0) {
      suspended_.erase(found);
    }
    if (Result<void> counted = remove_suspends(*store_, key, 1); !counted) {
      spdlog::error("governor: suspend record of pid {}: {}", key.pid, counted.error().message);
    }
  }
  return tally.text();
}

std::string Executor::kill(const governor::Command& command) {
  Tally tally(command.members.size());
  for (const governor::ProcessKey& key : command.members) {
    const Result<void> killed = control_->terminate(to_os(key), kill_exit_code);
    tally.add(key.pid, killed);
    if (!killed && killed.error().code != Errc::not_found) {
      continue;
    }
    saved_.erase(key);
    if (suspended_.erase(key) > 0) {
      if (Result<void> forgotten = forget_suspend(*store_, key); !forgotten) {
        spdlog::error("governor: suspend record of pid {}: {}", key.pid, forgotten.error().message);
      }
    }
  }
  return tally.text();
}

void Executor::resume_persisted(const governor::PersistedSuspend& suspend,
                                governor::Reason reason) {
  Tally tally(1);
  Result<void> resumed;
  for (int count = 0; count < suspend.count && resumed; ++count) {
    resumed = control_->resume(to_os(suspend.process));
  }
  tally.add(suspend.process.pid, resumed);
  if (resumed || resumed.error().code == Errc::not_found) {
    if (Result<void> forgotten = forget_suspend(*store_, suspend.process); !forgotten) {
      spdlog::error("governor: suspend record of pid {}: {}", suspend.process.pid,
                    forgotten.error().message);
    }
    suspended_.erase(suspend.process);
  }
  recorder_->record(GovernorEvent{.kind = EventKind::resume,
                                  .root = suspend.process,
                                  .step = governor::Step::suspend,
                                  .reason = reason,
                                  .members = 1,
                                  .detail = std::format("resumed {} times as recorded: {}",
                                                        suspend.count, tally.text())},
                    true);
}

int Executor::suspends(const governor::ProcessKey& process) const {
  const auto found = suspended_.find(process);
  return found == suspended_.end() ? 0 : found->second;
}

void Executor::clear() {
  saved_.clear();
  suspended_.clear();
  persisted_.clear();
}

void Executor::record(const governor::Command& command, std::string detail) {
  const std::optional<EventKind> kind = event_kind(command.kind);
  if (!kind) {
    return;
  }
  recorder_->record(GovernorEvent{.kind = *kind,
                                  .root = command.root,
                                  .root_image = command.root_image,
                                  .process_class = command.process_class,
                                  .step = command.step,
                                  .reason = command.reason,
                                  .actor = command.actor,
                                  .members = command.members.size(),
                                  .metrics = command.metrics,
                                  .detail = std::move(detail)},
                    mode_ == governor::GovernorMode::act);
}

}  // namespace myclean::governing
