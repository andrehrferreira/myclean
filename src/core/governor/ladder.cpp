#include "core/governor/ladder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/model/actor.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 4> choice_names = {"resume", "kill", "allow_once",
                                                          "always_allow"};
constexpr int cap_level = 3;  // steps 1-3 applied

Step throttle_step(int level) {
  return static_cast<Step>(level - 1);
}

bool has(const std::vector<Reason>& reasons, Reason reason) {
  return std::ranges::find(reasons, reason) != reasons.end();
}

bool memory_pressure(const std::vector<Reason>& reasons) {
  return has(reasons, Reason::commit) || has(reasons, Reason::available_memory) ||
         has(reasons, Reason::hard_faults);
}

// Build and test trees are the work the governor exists to hold back; another tree (a program the
// owner works in) takes a step only when no build or test tree can.
bool deferrable(rules::ProcessClass process_class) {
  return process_class == rules::ProcessClass::build || process_class == rules::ProcessClass::test;
}

Reason primary_reason(const std::vector<Reason>& reasons) {
  for (const Reason reason : {Reason::commit, Reason::available_memory, Reason::hard_faults}) {
    if (has(reasons, reason)) {
      return reason;
    }
  }
  return Reason::cpu;
}

template <typename Marks>
int level_of(const Marks& marks) {
  int level = 0;
  for (const auto& [key, mark] : marks) {
    level = std::max(level, mark.level);
  }
  return level;
}

template <typename Marks>
bool any_suspended(const Marks& marks) {
  return std::ranges::any_of(marks, [](const auto& entry) { return entry.second.suspended; });
}

// Drops the marks of processes with nothing applied any more.
template <typename Marks>
void drop_clean_marks(Marks& marks) {
  std::erase_if(
      marks, [](const auto& entry) { return entry.second.level == 0 && !entry.second.suspended; });
}

}  // namespace

std::string_view to_string(OwnerChoice choice) {
  return detail::enum_name(choice_names, choice);
}

std::optional<OwnerChoice> parse_owner_choice(std::string_view text) {
  return detail::enum_from_name<OwnerChoice>(choice_names, text);
}

void LadderEngine::set_policy(LadderPolicy policy) {
  policy_ = std::move(policy);
  notifications_.set_interval(policy_.notification_interval);
}

std::vector<Command> LadderEngine::tick(LadderInput input) {
  forest_ = build_forest(std::move(input.processes), input.foreground_pid, input.allowed_images);
  metrics_ = input.metrics;
  index_forest();
  const MonoTime now = input.now;
  if (paused_until_ && now >= *paused_until_) {
    paused_until_.reset();
  }
  std::erase_if(allow_once_, [&](const ProcessKey& root) { return !index_.contains(root); });
  std::erase_if(no_suspend_until_, [&](const auto& entry) {
    return !index_.contains(entry.first) || now >= entry.second;
  });
  notifications_.prune(now);

  std::vector<Command> out;
  rehome(out);
  if (input.pressure.state == PressureState::normal) {
    roll_back_all(Reason::pressure_cleared, Actor::governor(), false, out);
  }
  if (paused(now)) {
    return out;
  }
  catch_up(out);
  if (input.pressure.state == PressureState::pressure && !input.pressure.clearing) {
    escalate(input, out);
  }
  return out;
}

void LadderEngine::index_forest() {
  index_.clear();
  for (std::size_t index = 0; index < forest_.processes.size(); ++index) {
    index_.emplace(forest_.processes[index].key, index);
  }
  tree_of_.assign(forest_.processes.size(), no_tree);
  for (std::size_t tree = 0; tree < forest_.trees.size(); ++tree) {
    for (const std::size_t member : forest_.trees[tree].members) {
      tree_of_[member] = tree;
    }
  }
}

void LadderEngine::rehome(std::vector<Command>& out) {
  std::map<ProcessKey, TreeState> next;
  for (const auto& [root, state] : trees_) {
    TreeState released{.image = state.image,
                       .process_class = state.process_class,
                       .in_myclean_job = state.in_myclean_job};
    for (const auto& [key, mark] : state.marks) {
      const auto found = index_.find(key);
      if (found == index_.end()) {
        continue;  // exited
      }
      const std::size_t tree = tree_of_[found->second];
      if (tree == no_tree) {
        released.marks.emplace(key, mark);
        continue;
      }
      const ProcessKey now_root = forest_.processes[forest_.trees[tree].root].key;
      TreeState& target = next[now_root];
      target.origins.insert(state.origins.begin(), state.origins.end());
      if (now_root != root) {
        target.origins.emplace(root, state.image);
      }
      Mark& merged = target.marks[key];
      merged.level = std::max(merged.level, mark.level);
      merged.suspended = merged.suspended || mark.suspended;
      target.last_step = std::max(target.last_step, state.last_step);
      target.cap = target.cap ? target.cap : state.cap;
      target.killed = target.killed || state.killed;
    }
    release(root, released, Reason::protected_process, Actor::governor(), true, out);
  }
  for (auto& [root, state] : next) {
    describe_root(root, state);
  }
  trees_ = std::move(next);
}

void LadderEngine::roll_back_all(Reason reason, const Actor& actor, bool resume,
                                 std::vector<Command>& out) {
  for (auto& [root, state] : trees_) {
    release(root, state, reason, actor, resume, out);
    for (auto& [key, mark] : state.marks) {
      mark.level = 0;
      mark.suspended = mark.suspended && !resume;
    }
    drop_clean_marks(state.marks);
    state.cap.reset();
  }
  std::erase_if(trees_, [](const auto& entry) { return entry.second.marks.empty(); });
}

void LadderEngine::catch_up(std::vector<Command>& out) {
  for (auto& [root, state] : trees_) {
    const int level = level_of(state.marks);
    if (level == 0) {
      continue;
    }
    const Tree& tree = forest_.trees[tree_of_[index_.at(root)]];
    std::vector<ProcessKey> joined;
    for (const ProcessKey& key : member_keys(tree)) {
      if (!state.marks.contains(key)) {
        joined.push_back(key);
        state.marks[key].level = level;
      }
    }
    if (joined.empty()) {
      continue;
    }
    for (int applied = 1; applied <= level; ++applied) {
      Command step = command(CommandKind::apply_step, root, state, throttle_step(applied),
                             Reason::joined_tree);
      step.members = joined;
      if (step.step == Step::cap_job) {
        step.cap = state.cap;
      }
      out.push_back(std::move(step));
    }
  }
}

void LadderEngine::escalate(const LadderInput& input, std::vector<Command>& out) {
  const bool by_commit = memory_pressure(input.pressure.reasons);
  const Tree* chosen = nullptr;
  std::optional<Step> chosen_step;
  for (const Tree& tree : forest_.trees) {
    if (exempt(tree)) {
      continue;
    }
    const auto state = trees_.find(forest_.processes[tree.root].key);
    const std::optional<Step> step =
        next_step(tree, state == trees_.end() ? nullptr : &state->second, input.now, by_commit);
    if (!step) {
      continue;
    }
    const bool first = deferrable(forest_.processes[tree.root].process_class);
    const bool chosen_first =
        chosen != nullptr && deferrable(forest_.processes[chosen->root].process_class);
    const bool heavier =
        chosen == nullptr || (first && !chosen_first) ||
        (first == chosen_first &&
         ((by_commit ? tree.private_bytes > chosen->private_bytes
                     : tree.cpu_percent > chosen->cpu_percent) ||
          ((by_commit ? tree.private_bytes == chosen->private_bytes
                      : tree.cpu_percent == chosen->cpu_percent) &&
           forest_.processes[tree.root].key < forest_.processes[chosen->root].key)));
    if (heavier) {
      chosen = &tree;
      chosen_step = step;
    }
  }
  if (chosen == nullptr) {
    return;
  }
  const auto state = trees_.find(forest_.processes[chosen->root].key);
  const std::chrono::seconds cooldown = input.game_mode ? policy_.game_cooldown : policy_.cooldown;
  if (state != trees_.end() && state->second.last_step &&
      input.now - *state->second.last_step < cooldown) {
    return;
  }
  apply(*chosen, *chosen_step, primary_reason(input.pressure.reasons), input.now, out);
}

std::optional<Step> LadderEngine::next_step(const Tree& tree, const TreeState* state, MonoTime now,
                                            bool memory) const {
  if (state == nullptr) {
    return Step::priority;
  }
  if (state->killed) {
    return std::nullopt;
  }
  if (any_suspended(state->marks)) {
    return memory && kill_enabled(policy_, forest_.processes[tree.root].process_class)
               ? std::optional{Step::kill}
               : std::nullopt;
  }
  const rules::ProcessClass process_class = forest_.processes[tree.root].process_class;
  const int level = level_of(state->marks);
  if (level + 1 == cap_level && !cap_enabled(policy_, process_class)) {
    return std::nullopt;
  }
  if (level < cap_level) {
    return throttle_step(level + 1);
  }
  // Priority, EcoQoS and the cap settle a CPU shortage; only a memory shortage suspends.
  if (!memory || !suspend_enabled(policy_, process_class)) {
    return std::nullopt;
  }
  const bool blocked = std::ranges::any_of(tree.members, [&](std::size_t member) {
    const auto until = no_suspend_until_.find(forest_.processes[member].key);
    return until != no_suspend_until_.end() && now < until->second;
  });
  return blocked ? std::nullopt : std::optional{Step::suspend};
}

void LadderEngine::apply(const Tree& tree, Step step, Reason reason, MonoTime now,
                         std::vector<Command>& out) {
  const ProcessKey root = forest_.processes[tree.root].key;
  TreeState& state = trees_[root];
  describe_root(root, state);
  state.last_step = now;
  const std::vector<ProcessKey> members = member_keys(tree);

  Command applied = command(CommandKind::apply_step, root, state, step, reason);
  applied.members = members;
  bool notify = false;
  switch (step) {
    case Step::priority:
    case Step::ecoqos:
    case Step::cap_job: {
      if (step == Step::cap_job) {
        state.cap = cap_job_limits(tree.private_bytes, policy_);
        applied.cap = state.cap;
      }
      const int level = static_cast<int>(step) + 1;
      for (const ProcessKey& key : members) {
        state.marks[key].level = std::max(state.marks[key].level, level);
      }
      out.push_back(std::move(applied));
      notify = notifications_.allow(root, now);
      break;
    }
    case Step::suspend: {
      Command persist = command(CommandKind::persist_suspend, root, state, step, reason);
      persist.members = members;
      out.push_back(std::move(persist));
      out.push_back(std::move(applied));
      for (const ProcessKey& key : members) {
        state.marks[key].suspended = true;
      }
      notifications_.record(root, now);
      notify = true;
      break;
    }
    case Step::kill: {
      Command kill = command(CommandKind::kill, root, state, step, reason);
      kill.members = members;
      out.push_back(std::move(kill));
      state.killed = true;
      notify = notifications_.allow(root, now);
      break;
    }
  }
  if (notify) {
    Command notice = command(CommandKind::notify, root, state, step, reason);
    notice.members = members;
    out.push_back(std::move(notice));
  }
}

void LadderEngine::release(const ProcessKey& root, const TreeState& state, Reason reason,
                           const Actor& actor, bool resume, std::vector<Command>& out) const {
  Command roll_back = command(CommandKind::roll_back, root, state, Step::priority, reason);
  Command resumed = command(CommandKind::resume, root, state, Step::suspend, reason);
  int level = 0;
  for (const auto& [key, mark] : state.marks) {
    if (mark.level > 0) {
      roll_back.members.push_back(key);
      level = std::max(level, mark.level);
    }
    if (resume && mark.suspended) {
      resumed.members.push_back(key);
    }
  }
  if (level > 0) {
    roll_back.step = throttle_step(level);
    roll_back.actor = actor;
    out.push_back(std::move(roll_back));
  }
  if (!resumed.members.empty()) {
    resumed.actor = actor;
    out.push_back(std::move(resumed));
  }
}

Command LadderEngine::command(CommandKind kind, const ProcessKey& root, const TreeState& state,
                              Step step, Reason reason) const {
  return {.kind = kind,
          .root = root,
          .root_image = state.image,
          .process_class = state.process_class,
          .step = step,
          .reason = reason,
          .metrics = metrics_,
          .in_myclean_job = state.in_myclean_job};
}

std::vector<ProcessKey> LadderEngine::member_keys(const Tree& tree) const {
  std::vector<ProcessKey> keys;
  keys.reserve(tree.members.size());
  for (const std::size_t member : tree.members) {
    keys.push_back(forest_.processes[member].key);
  }
  return keys;
}

bool LadderEngine::exempt(const Tree& tree) const {
  return std::ranges::any_of(tree.members, [&](std::size_t member) {
    return allow_once_.contains(forest_.processes[member].key);
  });
}

std::vector<ProcessKey> LadderEngine::trees_from(const ProcessKey& root) const {
  std::vector<ProcessKey> roots;
  if (trees_.contains(root) || tree_rooted_at(root)) {
    roots.push_back(root);
  }
  for (const auto& [key, state] : trees_) {
    if (key != root && state.origins.contains(root)) {
      roots.push_back(key);
    }
  }
  return roots;
}

std::vector<Command> LadderEngine::choose(const ProcessKey& root, OwnerChoice choice,
                                          MonoTime now) {
  std::vector<Command> out;
  std::vector<ProcessKey> roots = trees_from(root);
  // The image the owner's Always allow is about: the old root's, when it moved on.
  std::string origin_image;
  for (const ProcessKey& tree : roots) {
    if (tree == root) {
      continue;
    }
    // A tree that came from `root` has a state: trees_from() found it there.
    const std::map<ProcessKey, std::string>& origins = trees_.at(tree).origins;
    if (const auto origin = origins.find(root); origin != origins.end()) {
      origin_image = origin->second;
    }
  }
  const bool moved = std::ranges::find(roots, root) == roots.end();
  if (roots.empty()) {
    roots.push_back(root);
  }
  for (const ProcessKey& tree : roots) {
    switch (choice) {
      case OwnerChoice::resume:
        resume_tree(tree, now, out);
        break;
      case OwnerChoice::kill:
        kill_tree(tree, out);
        break;
      case OwnerChoice::allow_once:
      case OwnerChoice::always_allow:
        allow_tree(tree, choice == OwnerChoice::always_allow && !moved, out);
        break;
    }
  }
  if (choice == OwnerChoice::always_allow && moved && !origin_image.empty()) {
    Command allow = command(CommandKind::add_to_allow_list, root, TreeState{.image = origin_image},
                            Step::priority, Reason::always_allow);
    allow.actor = Actor::owner();
    out.push_back(std::move(allow));
  }
  return out;
}

void LadderEngine::resume_failed(std::span<const ProcessKey> processes) {
  for (const ProcessKey& key : processes) {
    const auto found = index_.find(key);
    if (found == index_.end() || tree_of_[found->second] == no_tree) {
      continue;  // exited, or protected now
    }
    const ProcessKey root = forest_.processes[forest_.trees[tree_of_[found->second]].root].key;
    TreeState& state = trees_[root];
    describe_root(root, state);
    state.marks[key].suspended = true;
  }
}

bool LadderEngine::holds(const ProcessKey& root) const {
  return std::ranges::any_of(trees_from(root),
                             [&](const ProcessKey& tree) { return trees_.contains(tree); });
}

bool LadderEngine::suspends(const ProcessKey& root) const {
  return std::ranges::any_of(trees_from(root), [&](const ProcessKey& tree) {
    const auto state = trees_.find(tree);
    return state != trees_.end() && any_suspended(state->second.marks);
  });
}

void LadderEngine::resume_tree(const ProcessKey& root, MonoTime now, std::vector<Command>& out) {
  const auto state = trees_.find(root);
  if (state == trees_.end() || !any_suspended(state->second.marks)) {
    return;
  }
  Command resumed =
      command(CommandKind::resume, root, state->second, Step::suspend, Reason::resume);
  resumed.actor = Actor::owner();
  for (auto& [key, mark] : state->second.marks) {
    if (mark.suspended) {
      resumed.members.push_back(key);
      mark.suspended = false;
    }
  }
  out.push_back(std::move(resumed));
  drop_clean_marks(state->second.marks);
  if (state->second.marks.empty()) {
    trees_.erase(state);
  }
  no_suspend_until_[root] = now + policy_.resume_blocks_suspend;
}

void LadderEngine::kill_tree(const ProcessKey& root, std::vector<Command>& out) {
  const auto state = trees_.find(root);
  TreeState known = state != trees_.end() ? state->second : TreeState{};
  std::vector<ProcessKey> members;
  if (const std::optional<std::size_t> tree = tree_rooted_at(root)) {
    describe_root(root, known);
    members = member_keys(forest_.trees[*tree]);
  } else {
    std::ranges::transform(known.marks, std::back_inserter(members),
                           [](const auto& entry) { return entry.first; });
  }
  if (members.empty()) {
    return;
  }
  Command kill = command(CommandKind::kill, root, known, Step::kill, Reason::owner_kill);
  kill.actor = Actor::owner();
  kill.members = std::move(members);
  out.push_back(std::move(kill));
  if (state != trees_.end()) {
    state->second.killed = true;
  }
}

void LadderEngine::allow_tree(const ProcessKey& root, bool always, std::vector<Command>& out) {
  const Reason reason = always ? Reason::always_allow : Reason::allow_once;
  TreeState known;
  if (const auto state = trees_.find(root); state != trees_.end()) {
    known = state->second;
    release(root, known, reason, Actor::owner(), true, out);
    trees_.erase(state);
  } else {
    describe_root(root, known);
  }
  allow_once_.insert(root);
  if (always && !known.image.empty()) {
    Command allow = command(CommandKind::add_to_allow_list, root, known, Step::priority, reason);
    allow.actor = Actor::owner();
    out.push_back(std::move(allow));
  }
}

std::optional<std::size_t> LadderEngine::tree_rooted_at(const ProcessKey& root) const {
  const auto found = index_.find(root);
  if (found == index_.end() || tree_of_[found->second] == no_tree ||
      forest_.trees[tree_of_[found->second]].root != found->second) {
    return std::nullopt;
  }
  return tree_of_[found->second];
}

void LadderEngine::describe_root(const ProcessKey& root, TreeState& state) const {
  const auto found = index_.find(root);
  if (found == index_.end()) {
    return;
  }
  const ProcessNode& node = forest_.processes[found->second];
  state.image = node.image;
  state.process_class = node.process_class;
  state.in_myclean_job = node.in_myclean_job;
}

std::vector<Command> LadderEngine::pause(MonoTime now, std::chrono::seconds duration) {
  std::vector<Command> out;
  roll_back_all(Reason::pause, Actor::owner(), true, out);
  trees_.clear();
  paused_until_ = now + duration;
  return out;
}

std::vector<Command> LadderEngine::stall() {
  std::vector<Command> out;
  roll_back_all(Reason::telemetry_stalled, Actor::governor(), false, out);
  return out;
}

std::vector<Command> LadderEngine::stop() {
  std::vector<Command> out;
  roll_back_all(Reason::stop, Actor::governor(), true, out);
  trees_.clear();
  return out;
}

std::optional<Step> LadderEngine::step_of(const ProcessKey& root) const {
  const auto state = trees_.find(root);
  if (state == trees_.end()) {
    return std::nullopt;
  }
  if (state->second.killed) {
    return Step::kill;
  }
  if (any_suspended(state->second.marks)) {
    return Step::suspend;
  }
  const int level = level_of(state->second.marks);
  return level == 0 ? std::nullopt : std::optional{throttle_step(level)};
}

}  // namespace myclean::governor
