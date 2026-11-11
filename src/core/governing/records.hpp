#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/orphans.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/actor.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/rules/schema.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// The governor's tables (component "governor"):
//   governor_event    every decision and what came of it (kept 2 years like every event table)
//   governor_job      every job object the governor created and has not released: its JobRef
//                     (class and nonce), role, class, tree root and its image, creation time, so
//                     that a
//                     service restarted after a crash finds it again
//   governor_suspend  (pid, creation time, count) of the suspends the governor applied, each
//                     written before the suspend is applied
//   governor_saved    (pid, creation time) of each process whose priority, memory priority or
//                     EcoQoS the governor changed, with what it was before, written before the
//                     change, so that a service restarted after a crash restores it
//   governor_allow    the owner's Always-allow images
//   governor_orphan_rule  the owner's rules for patterns of orphaned polling loops: always end,
//                     or ignore
//   governor_pause    the time the owner's pause ends, while there is one
// Every intervention and rollback also goes to action_log as a GovernorStep entry (plan 0).
// Creation times are stored in nanoseconds since the Unix epoch (keys.hpp), other times in Unix
// seconds.
namespace myclean::governing {

[[nodiscard]] const store::MigrationSet& governor_migrations();
// governor_event for Store::history_retention; pass to Store::add_retention.
[[nodiscard]] store::RetentionRule governor_retention();

// What a governor_event row records. Stored as text (see core/ipc/governor.hpp).
enum class EventKind {
  // The ladder's commands (persist_suspend is part of the suspend it precedes).
  apply_step,
  roll_back,
  resume,
  kill,
  notify,
  add_to_allow_list,
  // Jobs.
  place_job,            // a build or test tree root put into its jobs on its start event
  job_released,         // a job lifted and made temporary: drained, rolled back or stopping
  job_adopted,          // a job of an earlier run re-opened, its limits set to the policy
  job_limits,           // the CPU cap of the build jobs changed with game mode
  notification_limit,   // the job's commit passed its notification limit
  job_memory_limit,     // a member's commit was refused by the job's commit cap
  commit_failure_exit,  // a member died of a refused commit (0xC0000017, 0xC0000409)
  untrusted_job,        // a persisted job name now owned by someone else: never adopted
  // Games.
  game_detected,  // fullscreen + GPU made a process a game
  game_mode,      // game mode turned on or off
  // Orphaned polling loops (the ends themselves are `kill` events with reason orphaned_loop).
  orphan_loops,  // a pattern proposed to the owner, or its trees ended by an `always` rule
  orphan_rule,   // the owner stored or forgot a rule for a pattern
};

[[nodiscard]] std::string_view to_string(EventKind kind);
[[nodiscard]] std::optional<EventKind> parse_event_kind(std::string_view text);
// The event kind of a ladder command; persist_suspend has none.
[[nodiscard]] std::optional<EventKind> event_kind(governor::CommandKind kind);

struct GovernorEvent {
  std::int64_t id = 0;  // the row id once stored
  WallTime time;
  EventKind kind = EventKind::apply_step;
  std::optional<governor::ProcessKey> root;
  std::string root_image;
  std::optional<rules::ProcessClass> process_class;
  std::optional<governor::Step> step;
  governor::Reason reason = governor::Reason::policy;
  Actor actor = Actor::governor();
  std::size_t members = 0;
  std::optional<governor::PressureSample> metrics;  // time is not stored
  std::string detail;
};

// Appends `event` to governor_event and, when `audit`, its GovernorStep entry to action_log, in
// one synchronous transaction. Returns the governor_event row id.
[[nodiscard]] Result<std::int64_t> save_event(store::Store& store, const GovernorEvent& event,
                                              bool audit);
// Stored events with time at or after `since`, newest first, at most `limit`.
[[nodiscard]] Result<std::vector<GovernorEvent>> load_events(store::Store& store, WallTime since,
                                                             std::size_t limit);

// One row of governor_job.
struct JobRecord {
  os::JobRef ref;
  governor::JobRole role = governor::JobRole::tree_job;
  rules::ProcessClass process_class = rules::ProcessClass::build;
  std::optional<governor::ProcessKey> root;
  std::string root_image;
  WallTime created;
};

[[nodiscard]] Result<void> save_job(store::Store& store, const JobRecord& job);
[[nodiscard]] Result<void> forget_job(store::Store& store, const os::JobRef& job);
// Every persisted job, oldest first. A row that no longer parses is skipped.
[[nodiscard]] Result<std::vector<JobRecord>> load_jobs(store::Store& store);

// Counts one more suspend of each process, in one transaction.
[[nodiscard]] Result<void> add_suspends(store::Store& store,
                                        std::span<const governor::ProcessKey> processes);
// Counts `times` fewer suspends of the process; the row goes when none is left.
[[nodiscard]] Result<void> remove_suspends(store::Store& store, const governor::ProcessKey& process,
                                           int times);
[[nodiscard]] Result<void> forget_suspend(store::Store& store, const governor::ProcessKey& process);
[[nodiscard]] Result<std::vector<governor::PersistedSuspend>> load_suspends(store::Store& store);

// One governor_saved row: what a process was before the governor changed it. `placed`: its
// priority before it was placed in a tree job at BELOW_NORMAL; the others: before ladder steps 1
// and 2. Stored as text (memory priority as its number, 1-5).
struct SavedState {
  governor::ProcessKey process;
  std::optional<os::PriorityClass> placed;
  std::optional<os::PriorityClass> priority;
  std::optional<os::MemoryPriority> memory;
  std::optional<os::EcoQos> eco_qos;

  [[nodiscard]] bool empty() const { return !placed && !priority && !memory && !eco_qos; }
  friend bool operator==(const SavedState&, const SavedState&) = default;
};

// Writes the states in one transaction, each replacing the process's row; an empty one removes
// it.
[[nodiscard]] Result<void> save_states(store::Store& store, std::span<const SavedState> states);
// Every row, by PID and creation time. A value that no longer parses reads as absent.
[[nodiscard]] Result<std::vector<SavedState>> load_states(store::Store& store);
// Removes every row (the clean stop restored them all).
[[nodiscard]] Result<void> forget_states(store::Store& store);

// The owner's pause: `until` is when it ends; none removes it.
[[nodiscard]] Result<void> save_pause(store::Store& store, const std::optional<WallTime>& until);
[[nodiscard]] Result<std::optional<WallTime>> load_pause(store::Store& store);

// Adds `image` (a file name; case is ignored) to the owner's allow list.
[[nodiscard]] Result<void> add_allowed(store::Store& store, std::string_view image, WallTime now);
[[nodiscard]] Result<std::vector<std::string>> load_allowed(store::Store& store);

// One row of governor_orphan_rule.
struct OrphanRuleRecord {
  std::int64_t id = 0;
  std::string pattern;  // governor::normalize_command_line
  std::string image;    // the roots' image when it was decided
  governor::OrphanRule rule = governor::OrphanRule::always;
  WallTime decided;
};

// Stores the rule of `pattern`, replacing an earlier one; returns its row id.
[[nodiscard]] Result<std::int64_t> save_orphan_rule(store::Store& store, std::string_view pattern,
                                                    std::string_view image,
                                                    governor::OrphanRule rule, WallTime now);
[[nodiscard]] Result<void> forget_orphan_rule(store::Store& store, std::int64_t id);
// Every stored rule, oldest first. A row whose rule no longer parses is skipped.
[[nodiscard]] Result<std::vector<OrphanRuleRecord>> load_orphan_rules(store::Store& store);

// Writes events (save_event) and keeps the newest in memory for governor.status. A failed write
// is kept as the latest problem and never stops the governor. Not thread-safe.
class Recorder {
 public:
  static constexpr std::size_t kept = 64;

  // `store` and `clock` must outlive the recorder.
  Recorder(store::Store& store, const os::IClock& clock) : store_(&store), clock_(&clock) {}

  // The pressure metrics of the current second, recorded with every event that has none of its
  // own.
  void set_metrics(const std::optional<governor::PressureSample>& metrics) { metrics_ = metrics; }

  // Sets the time, stores the event and keeps it. `audit`: also a GovernorStep in action_log.
  void record(GovernorEvent event, bool audit);

  // The newest events, newest first.
  [[nodiscard]] std::vector<GovernorEvent> recent(std::size_t limit) const;
  // The error of the latest failed write, until a write succeeds.
  [[nodiscard]] const std::optional<std::string>& problem() const { return problem_; }

 private:
  store::Store* store_;
  const os::IClock* clock_;
  std::optional<governor::PressureSample> metrics_;
  std::deque<GovernorEvent> recent_;  // newest first
  std::optional<std::string> problem_;
};

}  // namespace myclean::governing
