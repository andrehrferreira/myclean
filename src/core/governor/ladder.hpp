#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
//  4. Under pressure (not clearing, not paused), at most one step on one tree: the heaviest tree
//     that can take a step, by summed commit charge when a memory signal (available_memory,
//     commit, hard_faults) is among the reasons, by summed CPU otherwise (ties: lowest root
//     key). The reason recorded is the first of commit, available_memory, hard_faults, cpu
//     among the pressure reasons. A tree cannot take a step when it is exempt (Allow once or
//     Always allow on its root), killed, suspended without kill enabled for its root's class,
//     or at the cap-job step while its root is within the Resume block. When the chosen tree
//     had a step less than the cooldown ago (game mode: game_cooldown), nothing happens this
//     second: the ladder waits for it rather than moving on.
// Steps: priority, ecoqos and cap_job each with one apply_step (cap_job with its limits); suspend
// as persist_suspend then apply_step; kill only for a class in LadderPolicy::kill. Every step is
// followed by a notify the NotificationLimiter allows; the suspend's notify is always sent.
class LadderEngine {
 public:
  explicit LadderEngine(LadderPolicy policy)
      : policy_(std::move(policy)), notifications_(policy_.notification_interval) {}

  void set_policy(LadderPolicy policy);

  [[nodiscard]] std::vector<Command> tick(LadderInput input);

  // The owner's choice for the tree whose root is `root` (actor `owner`):
  //  - resume: resume the suspended processes; no suspend of that tree for
  //    resume_blocks_suspend; steps 1-3 stay and the ladder goes on.
  //  - kill: terminate the tree.
  //  - allow_once: roll back and resume the tree; no step on it until its root exits.
  //  - always_allow: as allow_once, plus add_to_allow_list for the root's image.
  [[nodiscard]] std::vector<Command> choose(const ProcessKey& root, OwnerChoice choice,
                                            MonoTime now);

  // Pause for `duration` (actor `owner`): every step is rolled back and every suspended process
  // resumed now; no step until the pause expires, then the ladder starts again by itself.
  [[nodiscard]] std::vector<Command> pause(MonoTime now, std::chrono::minutes duration);

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
  };

  static constexpr std::size_t no_tree = static_cast<std::size_t>(-1);

  void index_forest();
  void rehome(std::vector<Command>& out);
  void roll_back_all(Reason reason, const Actor& actor, bool resume, std::vector<Command>& out);
  void catch_up(std::vector<Command>& out);
  void escalate(const LadderInput& input, std::vector<Command>& out);
  [[nodiscard]] std::optional<Step> next_step(const Tree& tree, const TreeState* state,
                                              MonoTime now) const;
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
