#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/rules/schema.hpp"

// Crash reconciliation (governor spec, "Crash Recovery and Job Re-Adoption"; spike X4 section
// 5.5-5.6): what a service restarted after a crash does with the jobs and suspends it persisted.
namespace myclean::governor {

// What a persisted job is for. Stored as text: "class", "tree", "cap".
enum class JobRole {
  class_job,  // a class's parent job of job-free build or test trees
  tree_job,   // one build or test tree
  cap_job,    // ladder step 3
};

[[nodiscard]] std::string_view to_string(JobRole role);
[[nodiscard]] std::optional<JobRole> parse_job_role(std::string_view text);

struct PersistedJob {
  std::string name;  // Global\MyClean.Job.<class>.<nonce>
  rules::ProcessClass process_class = rules::ProcessClass::build;
  JobRole role = JobRole::tree_job;
};

// A persisted suspend: the process was suspended `count` times by the governor.
struct PersistedSuspend {
  ProcessKey process;
  int count = 0;
};

// The result of opening a persisted job by name.
enum class JobOpen {
  opened,     // opened, and its owner is SYSTEM
  missing,    // no such object any more
  untrusted,  // an object of that name whose owner is not SYSTEM: hostile, never adopted
};

struct ReopenedJob {
  std::string name;
  JobOpen outcome = JobOpen::missing;
  std::vector<std::uint32_t> member_pids;  // JobObjectBasicProcessIdList; opened only
};

struct RecoveryInput {
  std::vector<PersistedJob> jobs;
  std::vector<ReopenedJob> reopened;  // a persisted job not listed here counts as missing
  std::vector<PersistedSuspend> suspends;
  std::vector<ProcessKey> processes;  // the current process table
  // Persisted suspends the restarted service accounts for: it restored the tree's ladder state
  // and keeps the tree suspended for the owner. Every other live one is an orphan.
  std::vector<ProcessKey> accounted;
  bool game_mode = false;
};

enum class RecoveryKind {
  adopt_job,       // keep the job, set its limits to `limits`, track it by polling its members
  release_job,     // lift its limits, make it temporary again, close it, forget the record
  forget_job,      // the job is gone: forget the record
  untrusted_job,   // record a governor_event (reason untrusted_job), forget the record
  resume,          // resume `process` exactly `times` times, then forget its record
  forget_suspend,  // the process is gone (or the record is empty): forget the record
};

[[nodiscard]] std::string_view to_string(RecoveryKind kind);

struct RecoveryCommand {
  RecoveryKind kind = RecoveryKind::forget_job;
  std::string job_name;             // job commands
  std::optional<JobLimits> limits;  // adopt_job
  ProcessKey process;               // suspend commands
  int times = 0;                    // resume
  Reason reason = Reason::policy;
};

// Jobs: an opened job with members is adopted with its class's current limits (build CPU in
// game mode included), except a cap job, which the current policy never calls for after a crash
// (the ladder state is gone): it is released, as is any job without members. A missing job is
// forgotten; an untrusted one is reported and forgotten, never adopted.
// Suspends: a record whose (pid, creation time) is alive and not accounted for is resumed
// exactly `count` times with reason orphaned_suspend; a dead one is forgotten. Nothing that is
// not recorded is ever resumed. Commands come in input order, jobs first.
[[nodiscard]] std::vector<RecoveryCommand> plan_recovery(const RecoveryInput& input,
                                                         const JobPolicy& policy);

}  // namespace myclean::governor
