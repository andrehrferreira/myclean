#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "core/governing/job_book.hpp"
#include "core/governing/records.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/os/process_control.hpp"
#include "core/store/store.hpp"

// Carries out the ladder's per-process commands (governor spec, "Escalation Ladder", "Automatic
// Rollback", "Crash Recovery and Job Re-Adoption") on IProcessControl and the job book, and
// records each one in governor_event, and in act mode as a GovernorStep in action_log.
namespace myclean::governing {

// The exit code of a process the governor terminates (ladder step 5 or the owner's Kill).
inline constexpr std::uint32_t kill_exit_code = 1;

// Not thread-safe; the governor serializes every call.
class Executor {
 public:
  // Everything must outlive the executor.
  Executor(os::IProcessControl& control, JobBook& jobs, store::Store& store, Recorder& recorder,
           const os::IClock& clock)
      : control_(&control), jobs_(&jobs), store_(&store), recorder_(&recorder), clock_(&clock) {}

  // In observe mode nothing is applied or persisted; each command is only recorded, with detail
  // "observe".
  void set_mode(governor::GovernorMode mode) { mode_ = mode; }

  // Runs one apply_step, roll_back, persist_suspend, resume or kill command:
  //  - priority: BELOW_NORMAL and MEMORY_PRIORITY_LOW; ecoqos: EcoQoS on. The state before the
  //    first change of each process is read and saved in governor_saved before anything is
  //    changed (a process whose state cannot be read or saved is left as it is), and roll_back
  //    restores it.
  //  - cap_job: a root in a MyClean tree job has that job tightened to the cap (CPU and commit,
  //    whichever is lower); any other tree gets a cap job of its own, root first. roll_back
  //    restores the tree job's limits or lifts and releases the cap job.
  //  - persist_suspend writes the suspends to governor_suspend; the apply_step suspend that
  //    follows suspends exactly the processes whose record was written, and takes back the
  //    record of a process it could not suspend.
  //  - resume resumes only processes this executor suspended, once each, and counts the record
  //    down; kill terminates (exit code kill_exit_code) and forgets their records.
  // A process that exited meanwhile is skipped. notify and add_to_allow_list are the governor's.
  void execute(const governor::Command& command);

  // Resumes a persisted suspend exactly `count` times and forgets its record (crash recovery and
  // the clean stop). Recorded as a resume with `reason`.
  void resume_persisted(const governor::PersistedSuspend& suspend, governor::Reason reason);

  // Suspends this executor applied to the process and has not undone.
  [[nodiscard]] int suspends(const governor::ProcessKey& process) const;
  // The processes whose resume Windows refused (not because they exited) since the last call:
  // they stay suspended, and counted.
  [[nodiscard]] std::vector<governor::ProcessKey> take_failed_resumes();

  // A process placed in a tree job runs at BELOW_NORMAL (governor spec, "Proactive Build and Test
  // Jobs"): its priority before is saved in governor_saved first (an error, and nothing changed,
  // when it cannot be), then it is lowered. A process already placed is left as it is.
  [[nodiscard]] Result<void> lower_placed(const governor::ProcessKey& process);
  // Fail-open, after every ladder step was rolled back and every job released: every state still
  // saved is restored (the priority before placement included) and governor_saved is emptied.
  // Recorded as a roll_back with `reason`.
  void restore_all(governor::Reason reason);
  // Crash recovery, the governor_saved rows of an earlier run: a process of a job adopted again
  // (its PID in `adopted`) stays at BELOW_NORMAL and its priority before placement is kept saved;
  // every other value is restored and its row forgotten, each living process recorded as a
  // roll_back with `reason`. A value that could not be restored stays saved for the stop or the
  // next start.
  void restore_persisted(const std::vector<SavedState>& states,
                         const std::set<std::uint32_t>& adopted, governor::Reason reason);
  // The process exited (or was killed): what was saved of it and its suspend record are
  // forgotten.
  void forget(const governor::ProcessKey& process);

  // Forgets the saved states (after the clean stop).
  void clear();

 private:
  // What a process was before the governor's first change.
  struct Saved {
    std::optional<os::PriorityClass> priority;
    std::optional<os::MemoryPriority> memory;
    std::optional<os::EcoQos> eco_qos;
  };

  [[nodiscard]] std::string apply(const governor::Command& command);
  // Steps 1 and 2.
  [[nodiscard]] std::string throttle(const governor::Command& command);
  [[nodiscard]] std::string apply_cap(const governor::Command& command);
  [[nodiscard]] static std::string tighten(TrackedJob& job, const governor::JobLimits& cap);
  [[nodiscard]] std::string roll_back(const governor::Command& command);
  [[nodiscard]] std::string lift_caps(const governor::Command& command);
  [[nodiscard]] std::string persist(const governor::Command& command);
  [[nodiscard]] std::string suspend(const governor::Command& command);
  [[nodiscard]] std::string resume(const governor::Command& command);
  [[nodiscard]] std::string kill(const governor::Command& command);
  void record(const governor::Command& command, std::string detail);
  // Writes governor_saved for these processes from saved_ and placed_; a process with nothing
  // saved loses its row.
  [[nodiscard]] Result<void> save(std::span<const governor::ProcessKey> processes);
  // Sets the process back to what saved_ and placed_ hold of it (placed_ wins for the priority).
  [[nodiscard]] Result<void> restore(const governor::ProcessKey& process);
  // Sets each value given; the first failure.
  [[nodiscard]] Result<void> set_state(const governor::ProcessKey& process,
                                       std::optional<os::PriorityClass> priority,
                                       std::optional<os::MemoryPriority> memory,
                                       std::optional<os::EcoQos> eco_qos);

  os::IProcessControl* control_;
  JobBook* jobs_;
  store::Store* store_;
  Recorder* recorder_;
  const os::IClock* clock_;
  governor::GovernorMode mode_ = governor::GovernorMode::observe;
  std::map<governor::ProcessKey, Saved> saved_;
  std::map<governor::ProcessKey, os::PriorityClass> placed_;  // the priority before placement
  std::map<governor::ProcessKey, int> suspended_;
  std::set<governor::ProcessKey> persisted_;  // written by the latest persist_suspend
  std::vector<governor::ProcessKey> failed_resumes_;
};

}  // namespace myclean::governing
