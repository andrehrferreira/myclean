#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/jobs.hpp"
#include "core/rules/schema.hpp"
#include "core/store/store.hpp"

// The MyClean job objects the governor holds open (governor spec, "Proactive Build and Test
// Jobs", "Tree Membership Tracking"; spike X4 section 5): class parent jobs, tree jobs and the
// cap jobs of ladder step 3. Every job is persisted in governor_job before anything is assigned
// to it, and forgotten when it is released.
namespace myclean::governing {

// One job the governor holds open.
struct TrackedJob {
  std::unique_ptr<os::IJob> job;
  governor::JobRole role = governor::JobRole::tree_job;
  // Class and tree jobs: build or test. Cap jobs: the class of the tree's root.
  rules::ProcessClass process_class = rules::ProcessClass::build;
  std::optional<governor::ProcessKey> root;  // tree and cap jobs
  std::string root_image;
  governor::JobLimits limits;  // what the governor set
  // A tree job tightened by ladder step 3 instead of a cap job of its own: the limits to restore
  // when the step is rolled back.
  std::optional<governor::JobLimits> tightened_from;
  governor::CommitNotificationLimit notification{70};
  // From the completion port's messages, or from members() for a job without a port (one opened
  // again after a crash). A console process brings conhost.exe along: it is a member too.
  std::set<governor::ProcessKey> members;
  std::set<std::uint32_t> memory_limit_pids;  // job_memory_limit already recorded for these
  std::optional<Bytes> committed;             // at the latest poll
  bool adopted = false;                       // opened again after a crash: no port
};

// Not thread-safe; the governor serializes every call.
class JobBook {
 public:
  // Port messages drained per job and poll, so a notification flood costs a bounded amount.
  static constexpr std::size_t max_port_messages = 256;

  // Everything must outlive the book.
  JobBook(os::IJobControl& control, store::Store& store, Recorder& recorder)
      : control_(&control), store_(&store), recorder_(&recorder) {}

  // The share of the commit cap below which a cleared notification limit is set again.
  void set_rearm_percent(int percent) { rearm_percent_ = percent; }

  // A new, empty job: created, its limits set, persisted, tracked. Nothing is left behind when a
  // step fails.
  [[nodiscard]] Result<TrackedJob*> create(governor::JobRole role,
                                           rules::ProcessClass process_class,
                                           const std::optional<governor::ProcessKey>& root,
                                           std::string root_image,
                                           const governor::JobLimits& limits, WallTime now);
  // Tracks a job of an earlier run that was opened again, with the limits of the current policy.
  Result<TrackedJob*> adopt(std::unique_ptr<os::IJob> job, const JobRecord& record,
                            const governor::JobLimits& limits);

  // Sets every limit of the job: CPU hard cap, commit cap and the notification limit (cleared
  // when `limits` has none).
  [[nodiscard]] static Result<void> set_limits(TrackedJob& job, const governor::JobLimits& limits);

  // Fail-open for one job: lifts every limit, makes it temporary again, forgets its record and
  // stops tracking it (`job` is invalid afterwards). Records job_released with `reason` and
  // `detail`.
  void release(TrackedJob& job, governor::Reason reason, std::string_view detail);
  // release() for every job.
  void release_all(governor::Reason reason, std::string_view detail);

  // The 1 s work: drains every port (bounded), tracks membership, polls members() of jobs without
  // a port (PIDs resolved through `live`, pid -> creation time of the running processes),
  // records notification-limit, job-memory-limit and commit-failure exits, checks each job's
  // commit against its notification limit from accounting() (the message comes only while the job
  // is above the limit when it is processed) and re-arms the limit, and releases every job whose
  // last member exited.
  void poll(const std::map<std::uint32_t, WallTime>& live);
  // Drains the ports only, so that membership is current before a start is placed.
  void drain();

  // Where a starting process is: in no job, in one of MyClean's jobs, or in another program's.
  // Fails when the process cannot be queried (it exited).
  [[nodiscard]] Result<governor::JobMembership> membership(const governor::ProcessKey& process);
  // True when the process is a tracked member of a tree or class job.
  [[nodiscard]] bool in_tree_job(const governor::ProcessKey& process) const;

  // The class's parent job; null when there is none.
  [[nodiscard]] TrackedJob* class_job(rules::ProcessClass process_class);
  // The tree job whose members include `process`; null when none does.
  [[nodiscard]] TrackedJob* tree_job_of(const governor::ProcessKey& process);
  // Every cap job, and every tightened tree job, with a member among `processes`.
  [[nodiscard]] std::vector<TrackedJob*> caps_of(
      const std::vector<governor::ProcessKey>& processes);

  [[nodiscard]] const std::vector<std::unique_ptr<TrackedJob>>& jobs() const { return jobs_; }

 private:
  void drain(TrackedJob& job);
  void handle(TrackedJob& job, const os::JobEvent& event);
  void check_commit(TrackedJob& job, Bytes committed);
  void record(const TrackedJob& job, EventKind kind, governor::Reason reason, std::string detail);
  // Resolves the PIDs of a job without a port; false when they cannot be read.
  static bool poll_members(TrackedJob& job, const std::map<std::uint32_t, WallTime>& live);

  os::IJobControl* control_;
  store::Store* store_;
  Recorder* recorder_;
  int rearm_percent_ = 70;
  std::vector<std::unique_ptr<TrackedJob>> jobs_;
};

}  // namespace myclean::governing
