#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"

namespace myclean::os {

// A MyClean job object by its persisted identity: `Global\MyClean.Job.<job_class>.<nonce>`.
// `job_class` is 1-32 of [a-z0-9_-]; `nonce` is 32 lowercase hex digits, random per job.
struct JobRef {
  std::string job_class;
  std::string nonce;

  // The Win32 object name, `Global\MyClean.Job.<job_class>.<nonce>`.
  [[nodiscard]] std::string object_name() const;

  friend bool operator==(const JobRef&, const JobRef&) = default;
};

// invalid_argument when `job_class` or `nonce` is malformed.
[[nodiscard]] Result<void> validate(const JobRef& job);
// invalid_argument when `job_class` is malformed.
[[nodiscard]] Result<void> validate_job_class(std::string_view job_class);

// The limits a job currently carries; an absent value is not set.
struct JobLimits {
  // CPU hard cap (JobObjectCpuRateControlInformation, ENABLE | HARD_CAP) in percent of the whole
  // machine, at the API's resolution of 0.01 %. Another CPU rate mode reads as absent.
  std::optional<double> cpu_cap_percent;
  // Commit cap of the whole job (JOB_OBJECT_LIMIT_JOB_MEMORY).
  std::optional<Bytes> commit_cap;
  // Commit notification limit (JobObjectNotificationLimitInformation, JOB_MEMORY).
  std::optional<Bytes> commit_notification;

  friend bool operator==(const JobLimits&, const JobLimits&) = default;
};

// Accounting of a job since it was created. Judge CPU use from the difference of `cpu_time`
// over at least 2 s: the hard cap is enforced as an average over a cycle of about 0.6 s, and
// within it the job runs at full speed, then not at all (spike X4 §3.1).
struct JobAccounting {
  std::chrono::nanoseconds cpu_time{};  // user + kernel time of every process ever in the job
  Bytes committed;                      // current commit of the whole job
  Bytes peak_committed;                 // may exceed the cap after a refused commit
  std::uint32_t active_processes = 0;
  std::uint32_t total_processes = 0;  // every process that was ever a member
};

// The job's commit against its notification limit (JobObjectLimitViolationInformation).
struct LimitViolation {
  bool commit_exceeded = false;  // job commit is above the notification limit
  Bytes committed;               // job commit at the time of the query
  Bytes notification_limit;      // zero when no notification limit is set
};

enum class JobEventKind {
  new_process,          // a process joined the job (also synthesized for members that existed
                        // when the port was attached)
  exit_process,         // a member exited
  job_memory_limit,     // a commit of `pid` was refused by the commit cap
  notification_limit,   // the job's commit is above its notification limit; carries no PID
  active_process_zero,  // the last member exited
};

// One message of the job's completion port.
struct JobEvent {
  JobEventKind kind = JobEventKind::new_process;
  // The process for new_process, exit_process and job_memory_limit; 0 otherwise.
  std::uint32_t pid = 0;
  // new_process / exit_process: the creation time, when the process could still be opened and
  // was confirmed a member of this job. A process that exited before it was polled has none.
  std::optional<WallTime> created;
  // exit_process: the exit code, known only for a member that was seen alive by an earlier
  // new_process event of this poller.
  std::optional<std::uint32_t> exit_code;
  bool abnormal = false;        // exit_process: Windows reported an abnormal exit
  bool commit_failure = false;  // exit_process: is_commit_failure_exit(exit_code)
  // notification_limit: how many port messages this event stands for. One poll folds every
  // notification message it drains into one event (a capped build posts thousands per second,
  // spike X4 §3.7) and reads the violation once.
  std::uint32_t count = 1;
  std::optional<LimitViolation> violation;  // notification_limit
};

// One open MyClean job object. Every job is created without KILL_ON_JOB_CLOSE, BREAKAWAY_OK and
// SILENT_BREAKAWAY_OK, so closing it never ends a process and no member can leave it.
// Destroying the object only closes MyClean's handles: a permanent job and its limits stay, a
// non-permanent one disappears from the namespace with its last handle while its members keep
// running under its limits (spike X4 §3.5).
//
// poll() may run on its own thread while other methods run on another; release() must not
// overlap any other call. After release() every call fails with invalid_state.
class IJob {
 public:
  virtual ~IJob() = default;
  IJob(const IJob&) = delete;
  IJob& operator=(const IJob&) = delete;
  IJob(IJob&&) = delete;
  IJob& operator=(IJob&&) = delete;

  [[nodiscard]] virtual const JobRef& ref() const = 0;
  // True when the job is a permanent object that outlives every handle (OBJ_PERMANENT).
  [[nodiscard]] virtual bool permanent() const = 0;
  // True when this handle receives the job's port messages. A job can have one completion port
  // for its whole life, so a re-opened job created by an earlier run has none (spike X4 §3.4):
  // track its members with members() instead.
  [[nodiscard]] virtual bool has_port() const = 0;

  // Adds a process to the job; its future children join too. A member is assigned again
  // without error. A process already in another job nests this job under that one when this
  // job is still empty; a job holding processes of one job hierarchy refuses processes of
  // another with access_denied (spike X4 §3.3).
  [[nodiscard]] virtual Result<void> assign(const ProcessKey& process) = 0;
  [[nodiscard]] virtual Result<bool> contains(const ProcessKey& process) const = 0;

  // 0 < percent <= 100, else invalid_argument. Takes effect on a running job within one cycle.
  [[nodiscard]] virtual Result<void> set_cpu_cap(double percent) = 0;
  [[nodiscard]] virtual Result<void> lift_cpu_cap() = 0;
  // A commit beyond the cap fails inside the job (bad_alloc, NULL, ERROR_COMMITMENT_LIMIT);
  // Windows ends no process for it. `cap` must be non-zero.
  [[nodiscard]] virtual Result<void> set_commit_cap(Bytes cap) = 0;
  [[nodiscard]] virtual Result<void> lift_commit_cap() = 0;
  // Posts notification_limit events while the job's commit is above `limit` (non-zero). Clear
  // it after the first event and set it again once commit has fallen: a build above it posts
  // about one message per allocation.
  [[nodiscard]] virtual Result<void> set_commit_notification(Bytes limit) = 0;
  [[nodiscard]] virtual Result<void> clear_commit_notification() = 0;
  // Fail-open: no CPU rate control, no limit flags, no notification limit.
  [[nodiscard]] virtual Result<void> lift_all() = 0;

  [[nodiscard]] virtual Result<JobLimits> limits() const = 0;
  // The PIDs of the current members (JobObjectBasicProcessIdList).
  [[nodiscard]] virtual Result<std::vector<std::uint32_t>> members() const = 0;
  [[nodiscard]] virtual Result<JobAccounting> accounting() const = 0;
  [[nodiscard]] virtual Result<LimitViolation> limit_violation() const = 0;

  // The port messages waiting now, without blocking: at most `max_messages` messages are
  // drained per call (notification messages folded, see JobEvent::count), so a flood costs a
  // bounded amount per call. invalid_state when has_port() is false.
  [[nodiscard]] virtual Result<std::vector<JobEvent>> poll(std::size_t max_messages) = 0;

  // Makes the job temporary again (NtMakeTemporaryObject) and closes it, so it disappears once
  // no other handle is open. Limits are left as they are; lift them first when the members must
  // run free.
  [[nodiscard]] virtual Result<void> release() = 0;

 protected:
  IJob() = default;
};

// Creates and opens MyClean's job objects. Implemented by src/platform/jobs and tests/fakes.
class IJobControl {
 public:
  virtual ~IJobControl() = default;
  IJobControl(const IJobControl&) = delete;
  IJobControl& operator=(const IJobControl&) = delete;
  IJobControl(IJobControl&&) = delete;
  IJobControl& operator=(IJobControl&&) = delete;

  // A new, empty job `Global\MyClean.Job.<job_class>.<random nonce>` with its own completion
  // port. It is permanent when the process may create permanent objects (LocalSystem holds
  // SeCreatePermanentPrivilege) and an ordinary named job otherwise; see IJob::permanent().
  // Persist ref() to find the job again after a crash.
  [[nodiscard]] virtual Result<std::unique_ptr<IJob>> create(std::string_view job_class) = 0;

  // Opens an existing job by name. not_found when no such job exists; refused when its owner is
  // not the expected owner (SYSTEM in production): such a job was created by someone else under
  // MyClean's name and must not be adopted. A port is attached when the job never had one.
  [[nodiscard]] virtual Result<std::unique_ptr<IJob>> open(const JobRef& job) = 0;

  // True when the process is a member of any job, MyClean's or another program's.
  [[nodiscard]] virtual Result<bool> in_any_job(const ProcessKey& process) const = 0;

 protected:
  IJobControl() = default;
};

// Places a tree root into its tree job (spike X4 §5.1). With `parent` (the class parent job, for
// a root in no job), the root is assigned to `parent` first, so that `tree` nests under it; with
// none, `tree` nests under whatever job the root is already in (an IDE's), or under none.
[[nodiscard]] Result<void> assign_tree_root(const ProcessKey& root, IJob* parent, IJob& tree);

}  // namespace myclean::os
