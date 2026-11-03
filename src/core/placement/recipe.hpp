#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cleanup/command_runner.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/relocation.hpp"

// Relocation by a knowledge pack's recipe (packs::RelocationRecipe, docs/packs.md
// "relocate.toml"): the generic counterpart of RelocateAction, approval-only (tier ask) and run
// by the tray with the owner's token.
namespace myclean::placement {

// What the owner approved.
struct RecipeRelocation {
  packs::RelocationRecipe recipe;
  std::string instance;          // the instance's name; empty when the setting names none
  std::filesystem::path target;  // the new folder
  ContentMode content = ContentMode::move;  // env, config and registry; otherwise the tool moves
};

// A recipe as the JSON object of a plan's params, and back.
[[nodiscard]] nlohmann::json recipe_json(const packs::RelocationRecipe& recipe);
[[nodiscard]] Result<packs::RelocationRecipe> recipe_from_json(const nlohmann::json& json);

// Where the content of a recipe is now: one instance of it, or the recipe's one place.
struct RecipeLocation {
  std::string instance;  // the instance's name as the registry holds it; empty without instances
  std::filesystem::path path;
};

// Where the content of `recipe` is now: its one location, or one per instance (every registered
// instance that current.where keeps and skip_names does not name). A recipe whose location is
// not set and has no default has none. `tools` needs environment and files, and registry for a
// registry setting; `runner` runs a command setting.
[[nodiscard]] Result<std::vector<RecipeLocation>> recipe_locations(
    const Tools& tools, cleanup::ICommandRunner& runner, const packs::RelocationRecipe& recipe);

// Fails with in_use when an image of recipe.needs runs (tools.processes).
[[nodiscard]] Result<void> recipe_idle(const Tools& tools, const packs::RelocationRecipe& recipe);

// What apply runs or changes, one line each, for the plan the owner approves.
[[nodiscard]] std::vector<std::string> recipe_commands(const RecipeRelocation& relocation);

// Relocates what one recipe names (one instance of it, when its setting names instances):
//   locating  `current`: the variable, the configuration key (ini: the last `key = value` line;
//             json: a top-level string), the registry value or the path a command prints; when
//             it is not set, the first of `locations` (unless that is a pattern). An instance is
//             the subkey whose value `current.name` equals the approved name (any case), kept
//             by `current.where`, and not in `skip_names`: the name given to a command is the
//             one read from the registry, never the approved text;
//   preview   one item: the current folder, its identity and size;
//   apply     refuses when the location changed since the preview, the target overlaps it, an
//             image of `needs` runs, or the target's drive is not healthy or lacks room. Then by
//             target_kind:
//               env, config, registry  "move" copies the content and checks the copy holds the
//                        same files and bytes before the setting changes; the same setting gets
//                        the target (a variable keeps its type), and environment changes are
//                        announced. Undo data: the setting's previous and new value;
//               command  runs `command` with {target} and {name} (each placeholder inside one
//                        argument) through the command runner;
//               wsl-import  WSL 1: `wsl --export <name> <target>\<name>.tar`, `wsl --import
//                        <name>-myclean-move <target> <archive> --version 1`, then checks the
//                        import (its Lxss key has BasePath = the target, `wsl -d <temporary>
//                        -e true` succeeds) and only then `wsl --unregister <name>`; the copy
//                        then gets the original name (current.name) and DefaultUid. A failed
//                        import or check unregisters the copy and leaves the original
//                        registered. The archive stays in the target as a backup;
//             undo "none" makes the change not reversible, with the note as the reason;
//   verify    metric "relocated": 1 when the setting holds the target (env, config, registry)
//             or the location read again is the target (command, wsl-import);
//   undo      restore: writes the previous value back exactly (deleting a setting that was not
//             set); command and wsl-import: the same move with {target} = the previous folder.
//             Refuses, changing nothing, when the setting or location was changed meanwhile.
class RecipeAction final : public Action {
 public:
  // `tools` (with registry and processes as the recipe needs them) and `runner` must outlive
  // the action; `stop` cancels a command in progress.
  RecipeAction(const Tools& tools, cleanup::ICommandRunner& runner, RecipeRelocation relocation,
               std::stop_token stop)
      : tools_(&tools),
        runner_(&runner),
        relocation_(std::move(relocation)),
        stop_(std::move(stop)) {}

  [[nodiscard]] ActionInfo describe() const override;
  [[nodiscard]] Result<Preview> preview() override;
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

 private:
  const Tools* tools_;
  cleanup::ICommandRunner* runner_;
  RecipeRelocation relocation_;
  std::stop_token stop_;
};

}  // namespace myclean::placement
