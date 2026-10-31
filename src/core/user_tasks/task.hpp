#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"

// The user-scope plans the tray runs under the owner's token (cleaning spec, "Native Prune
// Commands Under the User Token"; placement spec): native prune commands, the disk-cleanup
// handlers that read the owner's profile, and relocations of caches and of what a knowledge pack's
// relocation recipe names. The service hands them out with
// user_tasks.next once the owner approved them, and records the report of user_tasks.report
// (contracts in core/ipc/plans.hpp).
namespace myclean::user_tasks {

enum class TaskKind { native, handler, relocate };
enum class Operation { apply, undo };

// One plan item the task may act on.
struct TaskItem {
  std::int64_t id = 0;
  // "native:<command>", "handler:<name>", "relocate:<cache>", "relocate:<pack>/<case>[:<name>]"
  std::filesystem::path path;
  Bytes bytes;
};

// The "task" of a user_tasks.next answer.
struct UserTask {
  std::int64_t plan = 0;
  std::string title;
  TaskKind kind = TaskKind::native;
  Operation operation = Operation::apply;
  Tier tier = Tier::ask;
  std::vector<TaskItem> items;                        // the items not rejected
  std::string command;                                // native: "pnpm store prune"
  std::filesystem::path volume;                       // handler: "C:\"
  std::vector<std::string> handlers;                  // handler: VolumeCaches key names
  placement::Relocation relocation;                   // relocate: a cache
  std::optional<placement::RecipeRelocation> recipe;  // relocate: by a recipe (then no cache)
  Undo undo;                                          // undo: the stored undo data, pieces joined
};

// The plan id of a task, readable even when the rest is not, so that a task the tray cannot read
// is still reported.
[[nodiscard]] Result<std::int64_t> task_plan(const nlohmann::json& task);
// A task: {"plan", "title", "kind": "native"|"handler"|"relocate", "operation": "apply"|"undo",
// "tier", "items": [{"id", "path", "bytes", "approved"}], "action": {"command"} |
// {"volume", "handlers"} | {"cache", "target", "content"} |
// {"recipe": placement::recipe_json(), "instance", "target", "content"},
// "undo": {"not_reversible", "items": [{"item", "data"}]} (undo only)}. Items the owner
// rejected (approved false) are dropped. Any other kind (a Recycle Bin "cleanup") is refused.
[[nodiscard]] Result<UserTask> parse_task(const nlohmann::json& task);

// The handlers that read the caller's own profile and so run in the tray (rule scope "user",
// docs/spikes/X3-cleanup-handlers.md). The tray runs no other handler, whatever it is asked.
[[nodiscard]] bool user_handler(std::string_view name);

// What running a task did.
struct TaskReport {
  std::int64_t plan = 0;
  // apply: verified, applied (verified worse and not reversible, or not measurable),
  // rolled_back (verified worse and undone) or failed; undo: rolled_back or failed.
  PlanState state = PlanState::failed;
  std::string error;  // why it failed, was rolled back, or was not verified; empty otherwise
  Undo undo;          // apply: the undo data per plan item, and why it cannot be undone
  std::optional<Verification> verification;
  std::vector<cleanup::Skipped> skipped;  // handlers left out, and why
};

// The `result` record of user_tasks.report: {"state", "error": text|null, "not_reversible"
// (when set), "undo": [{"item", "data"}] (when there is undo data), "verification" (when
// measured), "skipped" (when any)}. Texts are cut to the record's string size; undo data longer
// than that is sent in consecutive pieces of one item. Undo data that needs more pieces than a
// record list holds is left out and the change reported as not reversible, so that no partial
// undo is stored.
[[nodiscard]] nlohmann::json report_record(const TaskReport& report);
// One line for the dashboard and the log: "Plan 12 (Run `pnpm store prune`): verified".
[[nodiscard]] std::string summary(const TaskReport& report, std::string_view title);

// What the actions run with in the owner's context. Every pointer must outlive the runs; the
// handler host may be null for a task that is not a handler task. A recipe relocation also runs
// its commands with `runner` and reads placement.registry and placement.processes.
struct Toolbox {
  cleanup::ICommandRunner* runner = nullptr;
  cleanup::IHandlerHost* handlers = nullptr;
  const os::IFileSystem* file_system = nullptr;  // sizes of the native caches
  placement::Tools placement;
};

// Runs `task` to its end, never throwing:
//   apply  builds the action (refused, running nothing, when its command, handler or cache is
//          not one the tray allows), previews it, keeps the items not rejected (a native command
//          and a relocation act whole; handlers by item), applies and verifies; a worse result
//          is undone when the action can undo it;
//   undo   reverses the change with the task's undo data.
// `stop` cancels a command or handler in progress (the task then fails); a relocation that is
// copying runs to its end.
[[nodiscard]] TaskReport run(const UserTask& task, const Toolbox& tools,
                             const std::stop_token& stop);

}  // namespace myclean::user_tasks
