#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"

namespace myclean::fakes {

// The state of one simulated process.
struct FakeProcess {
  WallTime created;
  bool alive = true;
  std::optional<std::uint32_t> exit_code;
  os::PriorityClass priority = os::PriorityClass::normal;
  os::MemoryPriority memory_priority = os::MemoryPriority::normal;
  os::EcoQos eco_qos = os::EcoQos::automatic;
  std::uint32_t suspend_count = 0;
  std::vector<std::string> jobs;  // object names of its jobs, outermost first
};

// Processes and MyClean job objects in memory, with the Windows semantics the governor relies on
// (spike X4):
// - keys are verified: an exited process or a PID reused by a newer process is not_found;
// - a job joins the hierarchy of its first member; it then refuses a process from another
//   hierarchy (access_denied) and drags a job-free process into its hierarchy;
// - children inherit every job of their parent; the port of a job receives new_process,
//   exit_process (with exit code) and active_process_zero as members come and go;
// - a job has one completion port for life, attached at creation: an opened job has none, and
//   messages are dropped once the creating handle is closed;
// - a non-permanent job leaves the namespace with its last handle while its members keep it
//   (and its limits); a permanent one stays until release();
// - suspends are counted; a resume of a process that is not suspended changes nothing.
// Jobs and IJob handles are single-threaded here; every IJob must be destroyed before FakeJobs.
class FakeJobs final : public os::IJobControl, public os::IProcessControl {
 public:
  // `permanent`: whether created jobs are permanent (the service runs as LocalSystem).
  explicit FakeJobs(bool permanent = true);
  ~FakeJobs() override;
  FakeJobs(const FakeJobs&) = delete;
  FakeJobs& operator=(const FakeJobs&) = delete;
  FakeJobs(FakeJobs&&) = delete;
  FakeJobs& operator=(FakeJobs&&) = delete;

  // IJobControl. Nonces are 32 hex digits from a counter, so they are predictable in tests.
  [[nodiscard]] Result<std::unique_ptr<os::IJob>> create(std::string_view job_class) override;
  [[nodiscard]] Result<std::unique_ptr<os::IJob>> open(const os::JobRef& ref) override;
  [[nodiscard]] Result<bool> in_any_job(const os::ProcessKey& process) const override;

  // IProcessControl.
  [[nodiscard]] Result<os::PriorityClass> priority(const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::PriorityClass> set_priority(const os::ProcessKey& process,
                                                       os::PriorityClass priority) override;
  [[nodiscard]] Result<os::MemoryPriority> memory_priority(
      const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::MemoryPriority> set_memory_priority(
      const os::ProcessKey& process, os::MemoryPriority priority) override;
  [[nodiscard]] Result<os::EcoQos> eco_qos(const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::EcoQos> set_eco_qos(const os::ProcessKey& process,
                                               os::EcoQos state) override;
  [[nodiscard]] Result<void> suspend(const os::ProcessKey& process) override;
  [[nodiscard]] Result<void> resume(const os::ProcessKey& process) override;
  [[nodiscard]] Result<void> terminate(const os::ProcessKey& process,
                                       std::uint32_t exit_code) override;

  // Test setup. A process starts in no job; a PID can be reused once its process exited.
  // Throws std::logic_error when `pid` belongs to a live process.
  os::ProcessKey add_process(std::uint32_t pid, WallTime created);
  // A child of a live `parent`, in every job of the parent (posted as new_process).
  os::ProcessKey start_child(const os::ProcessKey& parent, std::uint32_t pid, WallTime created);
  // The process exits: exit_process to each of its jobs, active_process_zero to each job left
  // without a live member.
  void exit_process(const os::ProcessKey& process, std::uint32_t exit_code);
  // Throws std::logic_error for an unknown key.
  [[nodiscard]] const FakeProcess& process(const os::ProcessKey& process) const;

  // A job under a MyClean name created by someone else: open() refuses it.
  void plant_untrusted_job(const os::JobRef& ref);
  // True while the name can be opened.
  [[nodiscard]] bool job_exists(const os::JobRef& ref) const;
  // The limits of a job, whether or not a handle is open; throws for an unknown job.
  [[nodiscard]] const os::JobLimits& job_limits(const os::JobRef& ref) const;
  // CPU time and current commit reported by accounting() (peak follows the highest commit).
  void set_usage(const os::JobRef& ref, std::chrono::nanoseconds cpu_time, Bytes committed);
  // Queues any port message, such as job_memory_limit or notification_limit (when the job's
  // port is open; dropped otherwise, as Windows does).
  void post(const os::JobRef& ref, const os::JobEvent& event);
  // The next call of `operation` (a method name such as "assign", "set_cpu_cap", "suspend",
  // "create", "open", "priority") fails with `error`. Several queue up per operation.
  void fail_next(std::string operation, Error error);

 private:
  class Job;
  struct JobState {
    os::JobRef ref;
    bool trusted = true;
    bool permanent = false;
    bool named = true;  // still in the namespace
    int handles = 0;
    bool port_open = false;  // the creating handle, which owns the port, is still open
    bool placed = false;     // had its first member: its place in a hierarchy is fixed
    std::optional<std::string> parent;
    os::JobLimits limits;
    std::chrono::nanoseconds cpu_time{};
    Bytes committed;
    Bytes peak_committed;
    std::uint32_t total_processes = 0;
    std::deque<os::JobEvent> port;
  };

  [[nodiscard]] std::optional<Error> injected(std::string_view operation) const;
  [[nodiscard]] Result<FakeProcess*> live(const os::ProcessKey& process);
  [[nodiscard]] Result<const FakeProcess*> live(const os::ProcessKey& process) const;
  [[nodiscard]] JobState& state(const std::string& name);
  [[nodiscard]] const JobState& state(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> chain(const std::string& name) const;
  [[nodiscard]] std::uint32_t live_members(const std::string& name) const;
  void join(std::uint32_t pid, FakeProcess& process, const std::string& name);
  static void notify(JobState& job, const os::JobEvent& event);
  static void close(JobState& job, bool owned_port);

  bool permanent_;
  std::uint64_t next_nonce_ = 1;
  std::map<std::uint32_t, FakeProcess> processes_;
  std::map<std::string, JobState> jobs_;  // by object name
  mutable std::map<std::string, std::deque<Error>, std::less<>>
      failures_;  // consumed by const calls
};

}  // namespace myclean::fakes
