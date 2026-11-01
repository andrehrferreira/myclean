#pragma once

#include <optional>
#include <string_view>

#include "core/governor/policy.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

// Job decisions (governor spec, "Proactive Build and Test Jobs"; spike X4 section 5.1 and 5.3):
// where a starting build or test process goes, the limits of each kind of job, and the commit
// notification limit, which is handled once and re-armed later because Windows posts it about
// once per allocation while the job is above it.
namespace myclean::governor {

// The limits of one job object.
struct JobLimits {
  int cpu_percent = 100;              // hard cap, share of the machine
  Bytes commit;                       // JOB_OBJECT_LIMIT_JOB_MEMORY over the whole job
  std::optional<Bytes> notification;  // commit notification limit; absent: none
  friend bool operator==(const JobLimits&, const JobLimits&) = default;
};

// The limits of a build or test job (a tree job or the class's parent job): the class's caps,
// in game mode the build CPU cap at most JobPolicy::game_build_cpu_percent, and the
// notification limit at notification_percent of the commit cap. std::nullopt for every other
// class, which gets no proactive job.
[[nodiscard]] std::optional<JobLimits> class_job_limits(rules::ProcessClass process_class,
                                                        const JobPolicy& policy, bool game_mode);

// Ladder step 3: CPU at cap_cpu_percent, commit at the tree's current commit plus
// cap_commit_headroom_percent of it, never less than cap_commit_min_headroom above it.
[[nodiscard]] JobLimits cap_job_limits(Bytes tree_commit, const LadderPolicy& policy);

// The job a starting process is in, before the governor acts.
enum class JobMembership {
  none,     // no job
  myclean,  // a MyClean job (it inherited its parent's tree job)
  foreign,  // another program's job (an IDE's)
};

enum class JobPlacement {
  none,                     // leave it where it is
  class_job_then_tree_job,  // the class's parent job, then a new tree job, which nests under it
  tree_job,                 // a new tree job only, nesting under the foreign job
};

[[nodiscard]] std::string_view to_string(JobPlacement placement);

// A starting process of class build or test whose parent is not in a MyClean tree job becomes a
// tree root: in no job, it goes to the class job and then its tree job; in a foreign job, to a
// tree job only (one job cannot hold processes of two job hierarchies). Anything else, a process
// already in a MyClean job included, is left alone: children join their root's job by
// inheritance.
[[nodiscard]] JobPlacement place_job(rules::ProcessClass process_class, bool parent_in_myclean_job,
                                     JobMembership membership);

// One job's commit notification limit. Armed when the job is created; the first limit message
// is recorded as one governor event and clears the limit (later messages already queued are
// ignored), and the 1 s loop re-arms it once the job's commit is below rearm_percent of the cap.
class CommitNotificationLimit {
 public:
  enum class Action {
    none,
    record_and_clear,  // write one notification-limit event, set the job's limit to none
    rearm,             // set the job's notification limit again
  };

  explicit CommitNotificationLimit(int rearm_percent) : rearm_percent_(rearm_percent) {}

  // A JOB_OBJECT_MSG_NOTIFICATION_LIMIT message arrived for the job.
  [[nodiscard]] Action on_limit_message();
  // The job's committed memory at the 1 s loop, and its commit cap.
  [[nodiscard]] Action on_sample(Bytes commit, Bytes cap);

  [[nodiscard]] bool armed() const { return armed_; }

 private:
  int rearm_percent_;
  bool armed_ = true;
};

}  // namespace myclean::governor
