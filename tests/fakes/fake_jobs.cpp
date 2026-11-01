#include "fakes/fake_jobs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"

namespace myclean::fakes {

namespace {

// The live process `key` names in `processes` (const or not).
template <typename Processes>
auto find_live(Processes& processes, const os::ProcessKey& key)
    -> Result<decltype(&processes.begin()->second)> {
  const auto found = processes.find(key.pid);
  if (found == processes.end() || !found->second.alive) {
    return fail(Errc::not_found, std::format("no such process: pid {}", key.pid));
  }
  if (found->second.created != key.created) {
    return fail(Errc::not_found, std::format("pid {} now belongs to another process", key.pid));
  }
  return &found->second;
}

template <typename Jobs>
auto& find_job(Jobs& jobs, const std::string& name) {
  const auto found = jobs.find(name);
  if (found == jobs.end()) {
    throw std::logic_error("FakeJobs: unknown job " + name);
  }
  return found->second;
}

}  // namespace

class FakeJobs::Job final : public os::IJob {
 public:
  Job(FakeJobs& owner, os::JobRef ref, bool has_port)
      : owner_(owner), ref_(std::move(ref)), name_(ref_.object_name()), has_port_(has_port) {
    ++owner_.state(name_).handles;
  }
  ~Job() override {
    if (!released_) {
      FakeJobs::close(owner_.state(name_), has_port_);
    }
  }
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  Job(Job&&) = delete;
  Job& operator=(Job&&) = delete;

  [[nodiscard]] const os::JobRef& ref() const override { return ref_; }
  [[nodiscard]] bool permanent() const override {
    return !released_ && owner_.state(name_).permanent;
  }
  [[nodiscard]] bool has_port() const override { return has_port_; }

  [[nodiscard]] Result<void> assign(const os::ProcessKey& process) override {
    if (Result<void> ready = check("assign"); !ready) {
      return ready;
    }
    Result<FakeProcess*> found = owner_.live(process);
    if (!found) {
      return tl::unexpected(found.error());
    }
    FakeProcess& member = **found;
    if (std::ranges::find(member.jobs, name_) != member.jobs.end()) {
      return {};
    }
    JobState& job = owner_.state(name_);
    if (!job.placed) {
      job.placed = true;
      if (!member.jobs.empty()) {
        job.parent = member.jobs.back();
      }
      owner_.join(process.pid, member, name_);
      return {};
    }
    if (member.jobs.empty()) {
      for (const std::string& joined : owner_.chain(name_)) {
        owner_.join(process.pid, member, joined);
      }
      return {};
    }
    if (job.parent && member.jobs.back() == *job.parent) {
      owner_.join(process.pid, member, name_);
      return {};
    }
    return fail(Errc::access_denied,
                std::format("AssignProcessToJobObject(pid {}, {}): Win32 error 5 (another job "
                            "hierarchy)",
                            process.pid, name_));
  }

  [[nodiscard]] Result<bool> contains(const os::ProcessKey& process) const override {
    if (Result<void> ready = check("contains"); !ready) {
      return tl::unexpected(ready.error());
    }
    return owner_.live(process).transform([&](const FakeProcess* member) {
      return std::ranges::find(member->jobs, name_) != member->jobs.end();
    });
  }

  [[nodiscard]] Result<void> set_cpu_cap(double percent) override {
    if (Result<void> ready = check("set_cpu_cap"); !ready) {
      return ready;
    }
    if (std::isnan(percent) || percent <= 0.0 || percent > 100.0) {
      return fail(Errc::invalid_argument, std::format("CPU cap out of range: {}", percent));
    }
    owner_.state(name_).limits.cpu_cap_percent = std::round(percent * 100.0) / 100.0;
    return {};
  }

  [[nodiscard]] Result<void> lift_cpu_cap() override {
    return change("lift_cpu_cap", [](os::JobLimits& limits) { limits.cpu_cap_percent.reset(); });
  }

  [[nodiscard]] Result<void> set_commit_cap(Bytes cap) override {
    if (cap.count() == 0) {
      return fail(Errc::invalid_argument, "commit cap must be non-zero");
    }
    return change("set_commit_cap", [&](os::JobLimits& limits) { limits.commit_cap = cap; });
  }

  [[nodiscard]] Result<void> lift_commit_cap() override {
    return change("lift_commit_cap", [](os::JobLimits& limits) { limits.commit_cap.reset(); });
  }

  [[nodiscard]] Result<void> set_commit_notification(Bytes limit) override {
    if (limit.count() == 0) {
      return fail(Errc::invalid_argument, "commit notification limit must be non-zero");
    }
    return change("set_commit_notification",
                  [&](os::JobLimits& limits) { limits.commit_notification = limit; });
  }

  [[nodiscard]] Result<void> clear_commit_notification() override {
    return change("clear_commit_notification",
                  [](os::JobLimits& limits) { limits.commit_notification.reset(); });
  }

  [[nodiscard]] Result<void> lift_all() override {
    return change("lift_all", [](os::JobLimits& limits) { limits = {}; });
  }

  [[nodiscard]] Result<os::JobLimits> limits() const override {
    if (Result<void> ready = check("limits"); !ready) {
      return tl::unexpected(ready.error());
    }
    return owner_.state(name_).limits;
  }

  [[nodiscard]] Result<std::vector<std::uint32_t>> members() const override {
    if (Result<void> ready = check("members"); !ready) {
      return tl::unexpected(ready.error());
    }
    std::vector<std::uint32_t> pids;
    for (const auto& [pid, process] : owner_.processes_) {
      if (process.alive && std::ranges::find(process.jobs, name_) != process.jobs.end()) {
        pids.push_back(pid);
      }
    }
    return pids;
  }

  [[nodiscard]] Result<os::JobAccounting> accounting() const override {
    if (Result<void> ready = check("accounting"); !ready) {
      return tl::unexpected(ready.error());
    }
    const JobState& job = owner_.state(name_);
    return os::JobAccounting{.cpu_time = job.cpu_time,
                             .committed = job.committed,
                             .peak_committed = job.peak_committed,
                             .active_processes = owner_.live_members(name_),
                             .total_processes = job.total_processes};
  }

  [[nodiscard]] Result<os::LimitViolation> limit_violation() const override {
    if (Result<void> ready = check("limit_violation"); !ready) {
      return tl::unexpected(ready.error());
    }
    const JobState& job = owner_.state(name_);
    const Bytes limit = job.limits.commit_notification.value_or(Bytes{});
    return os::LimitViolation{
        .commit_exceeded = job.limits.commit_notification.has_value() && job.committed > limit,
        .committed = job.committed,
        .notification_limit = limit};
  }

  [[nodiscard]] Result<std::vector<os::JobEvent>> poll(std::size_t max_messages) override {
    if (Result<void> ready = check("poll"); !ready) {
      return tl::unexpected(ready.error());
    }
    if (!has_port_) {
      return fail(Errc::invalid_state, "job " + name_ + " has no completion port");
    }
    std::deque<os::JobEvent>& port = owner_.state(name_).port;
    std::vector<os::JobEvent> events;
    while (!port.empty() && events.size() < max_messages) {
      events.push_back(port.front());
      port.pop_front();
    }
    return events;
  }

  [[nodiscard]] Result<void> release() override {
    if (Result<void> ready = check("release"); !ready) {
      return ready;
    }
    JobState& job = owner_.state(name_);
    job.permanent = false;
    FakeJobs::close(job, has_port_);
    released_ = true;
    return {};
  }

 private:
  [[nodiscard]] Result<void> check(std::string_view operation) const {
    if (released_) {
      return fail(Errc::invalid_state, "job " + name_ + " was released");
    }
    if (std::optional<Error> error = owner_.injected(operation)) {
      return tl::unexpected(std::move(*error));
    }
    return {};
  }

  template <typename Change>
  Result<void> change(std::string_view operation, Change apply) {
    if (Result<void> ready = check(operation); !ready) {
      return ready;
    }
    apply(owner_.state(name_).limits);
    return {};
  }

  FakeJobs& owner_;
  os::JobRef ref_;
  std::string name_;
  bool has_port_;
  bool released_ = false;
};

FakeJobs::FakeJobs(bool permanent) : permanent_(permanent) {}

FakeJobs::~FakeJobs() = default;

Result<std::unique_ptr<os::IJob>> FakeJobs::create(std::string_view job_class) {
  if (std::optional<Error> error = injected("create")) {
    return tl::unexpected(std::move(*error));
  }
  if (Result<void> valid = os::validate_job_class(job_class); !valid) {
    return tl::unexpected(valid.error());
  }
  os::JobRef ref{.job_class = std::string(job_class),
                 .nonce = std::format("{:032x}", next_nonce_++)};
  JobState& job = jobs_[ref.object_name()];
  job.ref = ref;
  job.permanent = permanent_;
  job.port_open = true;
  return std::make_unique<Job>(*this, std::move(ref), true);
}

Result<std::unique_ptr<os::IJob>> FakeJobs::open(const os::JobRef& ref) {
  if (std::optional<Error> error = injected("open")) {
    return tl::unexpected(std::move(*error));
  }
  if (Result<void> valid = os::validate(ref); !valid) {
    return tl::unexpected(valid.error());
  }
  const std::string name = ref.object_name();
  const auto found = jobs_.find(name);
  if (found == jobs_.end() || !found->second.named) {
    return fail(Errc::not_found, "OpenJobObjectW(" + name + "): Win32 error 2");
  }
  const JobState& job = found->second;
  if (!job.trusted) {
    return fail(Errc::refused, "untrusted job " + name + ": owner is not the expected owner");
  }
  // MyClean's jobs get their one port at creation, so a reopened job has none.
  return std::make_unique<Job>(*this, ref, false);
}

Result<bool> FakeJobs::in_any_job(const os::ProcessKey& process) const {
  if (std::optional<Error> error = injected("in_any_job")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](const FakeProcess* found) { return !found->jobs.empty(); });
}

Result<os::PriorityClass> FakeJobs::priority(const os::ProcessKey& process) const {
  if (std::optional<Error> error = injected("priority")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](const FakeProcess* found) { return found->priority; });
}

Result<os::PriorityClass> FakeJobs::set_priority(const os::ProcessKey& process,
                                                 os::PriorityClass priority) {
  if (std::optional<Error> error = injected("set_priority")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform(
      [&](FakeProcess* found) { return std::exchange(found->priority, priority); });
}

Result<os::MemoryPriority> FakeJobs::memory_priority(const os::ProcessKey& process) const {
  if (std::optional<Error> error = injected("memory_priority")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](const FakeProcess* found) { return found->memory_priority; });
}

Result<os::MemoryPriority> FakeJobs::set_memory_priority(const os::ProcessKey& process,
                                                         os::MemoryPriority priority) {
  if (std::optional<Error> error = injected("set_memory_priority")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform(
      [&](FakeProcess* found) { return std::exchange(found->memory_priority, priority); });
}

Result<os::EcoQos> FakeJobs::eco_qos(const os::ProcessKey& process) const {
  if (std::optional<Error> error = injected("eco_qos")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](const FakeProcess* found) { return found->eco_qos; });
}

Result<os::EcoQos> FakeJobs::set_eco_qos(const os::ProcessKey& process, os::EcoQos state) {
  if (std::optional<Error> error = injected("set_eco_qos")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform(
      [&](FakeProcess* found) { return std::exchange(found->eco_qos, state); });
}

Result<void> FakeJobs::suspend(const os::ProcessKey& process) {
  if (std::optional<Error> error = injected("suspend")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](FakeProcess* found) { ++found->suspend_count; });
}

Result<void> FakeJobs::resume(const os::ProcessKey& process) {
  if (std::optional<Error> error = injected("resume")) {
    return tl::unexpected(std::move(*error));
  }
  return live(process).transform([](FakeProcess* found) {
    if (found->suspend_count > 0) {
      --found->suspend_count;
    }
  });
}

Result<void> FakeJobs::terminate(const os::ProcessKey& process, std::uint32_t exit_code) {
  if (std::optional<Error> error = injected("terminate")) {
    return tl::unexpected(std::move(*error));
  }
  if (Result<FakeProcess*> found = live(process); !found) {
    return tl::unexpected(found.error());
  }
  exit_process(process, exit_code);
  return {};
}

os::ProcessKey FakeJobs::add_process(std::uint32_t pid, WallTime created) {
  if (const auto found = processes_.find(pid); found != processes_.end() && found->second.alive) {
    throw std::logic_error(std::format("FakeJobs: pid {} is still running", pid));
  }
  processes_.insert_or_assign(pid, FakeProcess{.created = created});
  return os::ProcessKey{.pid = pid, .created = created};
}

os::ProcessKey FakeJobs::start_child(const os::ProcessKey& parent, std::uint32_t pid,
                                     WallTime created) {
  Result<FakeProcess*> found = live(parent);
  if (!found) {
    throw std::logic_error("FakeJobs: parent is not running: " + found.error().message);
  }
  const std::vector<std::string> jobs = (*found)->jobs;
  const os::ProcessKey child = add_process(pid, created);
  FakeProcess& process = processes_.at(pid);
  for (const std::string& name : jobs) {
    join(pid, process, name);
  }
  return child;
}

void FakeJobs::exit_process(const os::ProcessKey& process, std::uint32_t exit_code) {
  Result<FakeProcess*> found = live(process);
  if (!found) {
    throw std::logic_error("FakeJobs: process is not running: " + found.error().message);
  }
  FakeProcess& exiting = **found;
  exiting.alive = false;
  exiting.exit_code = exit_code;
  for (const std::string& name : exiting.jobs) {
    JobState& job = state(name);
    notify(job, os::JobEvent{.kind = os::JobEventKind::exit_process,
                             .pid = process.pid,
                             .created = process.created,
                             .exit_code = exit_code,
                             .commit_failure = os::is_commit_failure_exit(exit_code)});
    if (live_members(name) == 0) {
      notify(job, os::JobEvent{.kind = os::JobEventKind::active_process_zero});
    }
  }
}

const FakeProcess& FakeJobs::process(const os::ProcessKey& process) const {
  const auto found = processes_.find(process.pid);
  if (found == processes_.end() || found->second.created != process.created) {
    throw std::logic_error(std::format("FakeJobs: unknown process {}", process.pid));
  }
  return found->second;
}

void FakeJobs::plant_untrusted_job(const os::JobRef& ref) {
  JobState& job = jobs_[ref.object_name()];
  job.ref = ref;
  job.trusted = false;
  job.permanent = true;
}

bool FakeJobs::job_exists(const os::JobRef& ref) const {
  const auto found = jobs_.find(ref.object_name());
  return found != jobs_.end() && found->second.named;
}

const os::JobLimits& FakeJobs::job_limits(const os::JobRef& ref) const {
  return state(ref.object_name()).limits;
}

void FakeJobs::set_usage(const os::JobRef& ref, std::chrono::nanoseconds cpu_time,
                         Bytes committed) {
  JobState& job = state(ref.object_name());
  job.cpu_time = cpu_time;
  job.committed = committed;
  job.peak_committed = std::max(job.peak_committed, committed);
}

void FakeJobs::post(const os::JobRef& ref, const os::JobEvent& event) {
  notify(state(ref.object_name()), event);
}

void FakeJobs::fail_next(std::string operation, Error error) {
  failures_[std::move(operation)].push_back(std::move(error));
}

std::optional<Error> FakeJobs::injected(std::string_view operation) const {
  const auto found = failures_.find(operation);
  if (found == failures_.end() || found->second.empty()) {
    return std::nullopt;
  }
  Error error = std::move(found->second.front());
  found->second.pop_front();
  return error;
}

Result<FakeProcess*> FakeJobs::live(const os::ProcessKey& process) {
  return find_live(processes_, process);
}

Result<const FakeProcess*> FakeJobs::live(const os::ProcessKey& process) const {
  return find_live(processes_, process);
}

FakeJobs::JobState& FakeJobs::state(const std::string& name) {
  return find_job(jobs_, name);
}

const FakeJobs::JobState& FakeJobs::state(const std::string& name) const {
  return find_job(jobs_, name);
}

std::vector<std::string> FakeJobs::chain(const std::string& name) const {
  std::vector<std::string> jobs{name};
  for (std::optional<std::string> parent = state(name).parent; parent;
       parent = state(*parent).parent) {
    jobs.insert(jobs.begin(), *parent);
  }
  return jobs;
}

std::uint32_t FakeJobs::live_members(const std::string& name) const {
  return static_cast<std::uint32_t>(std::ranges::count_if(processes_, [&](const auto& entry) {
    return entry.second.alive &&
           std::ranges::find(entry.second.jobs, name) != entry.second.jobs.end();
  }));
}

void FakeJobs::join(std::uint32_t pid, FakeProcess& process, const std::string& name) {
  process.jobs.push_back(name);
  JobState& job = state(name);
  ++job.total_processes;
  notify(job, os::JobEvent{
                  .kind = os::JobEventKind::new_process, .pid = pid, .created = process.created});
}

void FakeJobs::notify(JobState& job, const os::JobEvent& event) {
  if (job.port_open) {
    job.port.push_back(event);
  }
}

void FakeJobs::close(JobState& job, bool owned_port) {
  --job.handles;
  if (owned_port) {
    job.port_open = false;
    job.port.clear();
  }
  if (job.handles == 0 && !job.permanent) {
    job.named = false;
  }
}

}  // namespace myclean::fakes
