#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/notify.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/pressure.hpp"
#include "core/model/units.hpp"

// The escalation ladder (governor spec, "Escalation Ladder", "Automatic Rollback", "Owner
// Exemptions Survive Continued Pressure", "Pause Governor", "Fail-Open on Clean Stop"): one
// snapshot per second in, the commands for the service out. Pure and deterministic; time comes
// from the snapshot.
namespace myclean::governor {

// What the service knows at one second.
struct LadderInput {
  MonoTime now;
  PressureStatus pressure;  // PressureDetector::update of this second
  PressureSample metrics;   // the sample it was given, recorded in every command
  std::vector<ProcessNode> processes;
  std::optional<std::uint32_t> foreground_pid;
  bool game_mode = false;  // GameMode::active: the game-mode cooldown applies
  // The owner's Always-allow images, protected like class allow until the class set has them.
  std::vector<std::string> allowed_images;
};

// The owner's answer to a notification. Stored as text.
enum class OwnerChoice { resume, kill, allow_once, always_allow };

[[nodiscard]] std::string_view to_string(OwnerChoice choice);
[[nodiscard]] std::optional<OwnerChoice> parse_owner_choice(std::string_view text);

// Each second (tick):
//  1. The forest is rebuilt (build_forest). Processes the governor acted on follow their tree:
//     a process now in another tree (its tree's root exited, or the root's protected parent
//     stopped being protected) carries its steps there; a process that became protected (the
//     owner brought it to the foreground, say) has its steps rolled back and is resumed
//     (reason `protected`).
//  2. In the normal state, steps 1-3 of every tree are rolled back (reason pressure_cleared);
//     suspended processes stay suspended until the owner acts.
//  3. A process that joined a throttled tree gets the tree's steps 1-3 (reason joined_tree); it
//     is not suspended, since the owner's notification was about the tree as it was.
//  4. Under pressure (not clearing, not paused), at most one step on one tree: among the trees
//     that can take a step, a build or test tree first (another tree only when none can), and
//     among those the heaviest, by summed commit charge when a memory signal (available_memory,
//     commit, hard_faults) is among the reasons, by summed CPU otherwise (ties: lowest root
//     key). The reason recorded is the first of commit, available_memory, hard_faults, cpu
//     among the pressure reasons. A tree cannot take a step when it is exempt (Allow once or
//     Always allow on its root), killed, at EcoQoS without cap enabled for its root's class, at
//     the cap without suspend enabled for it or without a memory signal among the reasons,
//     suspended without kill enabled for its class or without a memory signal, or at the
//     cap-job step while its root is within the Resume block. When the chosen
//     tree had a step less than the cooldown ago (game mode: game_cooldown), nothing happens this
//     second: the ladder waits for it rather than moving on.
// A tree's processes keep what the governor did to them when they move to other trees (its root
// exited, say): the owner's choice for the old root then acts on the trees they are in now.
// Steps: priority, ecoqos and cap_job each with one apply_step (cap_job with its limits); suspend
// as persist_suspend then apply_step; kill only for a class in LadderPolicy::kill. Every step is
// followed by a notify the NotificationLimiter allows; the suspend's notify is always sent.
class LadderEngine {
 public:
  explicit LadderEngine(LadderPolicy policy)
      : policy_(std::move(policy)), notifications_(policy_.notification_interval) {}

  void set_policy(LadderPolicy policy);

  [[nodiscard]] std::vector<Command> tick(LadderInput input);

  // The owner's choice for the tree whose root is `root`, and for the trees its processes moved
  // to since (actor `owner`):
  //  - resume: resume the suspended processes; no suspend of that tree for
  //    resume_blocks_suspend; steps 1-3 stay and the ladder goes on.
  //  - kill: terminate the tree.
  //  - allow_once: roll back and resume the tree; no step on it until its root exits.
  //  - always_allow: as allow_once, plus add_to_allow_list for the root's image.
  [[nodiscard]] std::vector<Command> choose(const ProcessKey& root, OwnerChoice choice,
                                            MonoTime now);

  // The OS refused to resume these processes: they stay marked suspended in the trees they are
  // in, so the owner's Resume (or a retry) can still reach them.
  void resume_failed(std::span<const ProcessKey> processes);

  // Whether the tree whose root is `root`, or a tree its processes moved to, still has a step in
  // effect (holds) or a suspended process (suspends).
  [[nodiscard]] bool holds(const ProcessKey& root) const;
  [[nodiscard]] bool suspends(const ProcessKey& root) const;

  // Pause for `duration` (actor `owner`): every step is rolled back and every suspended process
  // resumed now; no step until the pause expires, then the ladder starts again by itself.
  [[nodiscard]] std::vector<Command> pause(MonoTime now, std::chrono::seconds duration);

  // Telemetry stalled, so the ladder gets no snapshot: steps 1-3 of every tree are rolled back
  // (reason telemetry_stalled); suspended processes stay suspended until the owner acts.
  [[nodiscard]] std::vector<Command> stall();

  // Clean stop: roll back and resume everything (reason stop).
  [[nodiscard]] std::vector<Command> stop();

  [[nodiscard]] bool paused(MonoTime now) const { return paused_until_ && now < *paused_until_; }

  // The highest step in effect on the tree with this root: suspend while any of its processes
  // is suspended, kill after a kill, else the highest of steps 1-3; std::nullopt when none.
  [[nodiscard]] std::optional<Step> step_of(const ProcessKey& root) const;

  // The forest of the latest tick.
  [[nodiscard]] const Forest& forest() const { return forest_; }

 private:
  // What the governor did to one process.
  struct Mark {
    int level = 0;  // steps 1-3 applied: 0 none, 1 priority, 2 ecoqos, 3 cap_job
    bool suspended = false;
  };
  // A tree the governor acted on, by its root.
  struct TreeState {
    std::map<ProcessKey, Mark> marks;
    std::optional<MonoTime> last_step;
    std::optional<JobLimits> cap;
    bool killed = false;
    std::string image;  // the root's
    rules::ProcessClass process_class = rules::ProcessClass::normal;
    bool in_myclean_job = false;
    // The roots (and their images) of earlier trees whose processes moved into this one.
    std::map<ProcessKey, std::string> origins;
  };

  static constexpr std::size_t no_tree = static_cast<std::size_t>(-1);

  void index_forest();
  void rehome(std::vector<Command>& out);
  void roll_back_all(Reason reason, const Actor& actor, bool resume, std::vector<Command>& out);
  void catch_up(std::vector<Command>& out);
  void escalate(const LadderInput& input, std::vector<Command>& out);
  // `memory`: a memory signal is among the pressure reasons (suspend and kill need one).
  [[nodiscard]] std::optional<Step> next_step(const Tree& tree, const TreeState* state,
                                              MonoTime now, bool memory) const;
  void apply(const Tree& tree, Step step, Reason reason, MonoTime now, std::vector<Command>& out);
  void release(const ProcessKey& root, const TreeState& state, Reason reason, const Actor& actor,
               bool resume, std::vector<Command>& out) const;
  void resume_tree(const ProcessKey& root, MonoTime now, std::vector<Command>& out);
  void kill_tree(const ProcessKey& root, std::vector<Command>& out);
  void allow_tree(const ProcessKey& root, bool always, std::vector<Command>& out);
  // The index in forest_.trees of the tree whose root is `root`, if `root` is one.
  [[nodiscard]] std::optional<std::size_t> tree_rooted_at(const ProcessKey& root) const;
  // Copies the root's image, class and job flag from the forest, when it is in it.
  void describe_root(const ProcessKey& root, TreeState& state) const;
  [[nodiscard]] Command command(CommandKind kind, const ProcessKey& root, const TreeState& state,
                                Step step, Reason reason) const;
  [[nodiscard]] std::vector<ProcessKey> member_keys(const Tree& tree) const;
  [[nodiscard]] bool exempt(const Tree& tree) const;
  // `root` when it has a state or is a tree root now, and every tree that came from it.
  [[nodiscard]] std::vector<ProcessKey> trees_from(const ProcessKey& root) const;

  LadderPolicy policy_;
  NotificationLimiter notifications_;
  Forest forest_;
  std::map<ProcessKey, std::size_t> index_;  // process -> index in forest_.processes
  std::vector<std::size_t> tree_of_;         // per process: index in forest_.trees, or no_tree
  PressureSample metrics_;
  std::map<ProcessKey, TreeState> trees_;
  std::set<ProcessKey> allow_once_;                  // roots exempt until they exit
  std::map<ProcessKey, MonoTime> no_suspend_until_;  // roots the owner resumed
  std::optional<MonoTime> paused_until_;
};

}  // namespace myclean::governor
