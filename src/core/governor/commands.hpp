#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/pressure.hpp"
#include "core/governor/reason.hpp"
#include "core/model/actor.hpp"
#include "core/rules/schema.hpp"

// What the ladder engine asks the service to do: plain data, one command per OS operation on a
// tree, carrying everything a governor_event row and its GovernorStep action_log entry record.
namespace myclean::governor {

// The ladder (governor spec, "Escalation Ladder"), in order. Stored as text.
enum class Step {
  priority,  // priority class BELOW_NORMAL and memory priority MEMORY_PRIORITY_LOW
  ecoqos,    // EcoQoS execution-speed throttling
  cap_job,   // a cap job (Command::cap holds its limits)
  suspend,   // suspend every process of the tree
  kill,      // terminate every process of the tree
};

[[nodiscard]] std::string_view to_string(Step step);
[[nodiscard]] std::optional<Step> parse_step(std::string_view text);

enum class CommandKind {
  apply_step,        // apply `step` to every member
  roll_back,         // undo, on every member, each step up to `step` that was applied to it
                     // (steps 1-3 only; the service restores what it saved before applying)
  persist_suspend,   // store (pid, creation time) of every member as suspended once more;
                     // always right before the apply_step `suspend` it belongs to
  resume,            // resume every member once (each was suspended by the governor)
  kill,              // terminate every member (ladder step 5 or the owner's Kill)
  notify,            // show the owner the notification for the tree: Resume, Kill, Allow once,
                     // Always allow
  add_to_allow_list  // add `root_image` to the owner's allow class persistently
};

[[nodiscard]] std::string_view to_string(CommandKind kind);

struct Command {
  CommandKind kind = CommandKind::apply_step;
  Actor actor = Actor::governor();  // `owner` for what the owner chose
  ProcessKey root;
  std::string root_image;
  rules::ProcessClass process_class = rules::ProcessClass::normal;  // the root's
  std::vector<ProcessKey> members;  // the processes the command acts on
  Step step = Step::priority;  // apply_step, roll_back: see above; resume: suspend; kill: kill;
                               // notify: the step that caused it; persist_suspend: suspend
  Reason reason = Reason::cpu;
  PressureSample metrics;        // the pressure metrics of the second it was decided in
  std::optional<JobLimits> cap;  // apply_step cap_job: the cap job's limits
  bool in_myclean_job = false;   // the root is in a MyClean tree job (step 3 may tighten it)
};

}  // namespace myclean::governor
