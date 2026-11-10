#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/cleaning/progress.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/indexer/scope.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "core/startup/startup.hpp"

// The cleaning executor (cleaning spec C4): turns what the rules select into plans by tier and
// scope, and runs the approved service-scope plans through the action framework.
namespace myclean::cleaning {

// The machine as the executor sees it. Every pointer must outlive the executor; a null one
// disables what needs it (no git: no developer artifacts; no handlers: no handler sizes or
// purges; no bin: no aged Recycle Bin items; no drives: every item is quarantined on its own
// volume).
struct Probes {
  const clean::IFileTree* tree = nullptr;
  const clean::IEnvironment* environment = nullptr;  // the owner's variables
  const clean::IUsage* usage = nullptr;
  const clean::IGit* git = nullptr;
  cleanup::IHandlerHost* handlers = nullptr;    // system-area handlers, run by the service
  const cleanup::IBinRecords* bin = nullptr;    // the owner's $I records: aged Recycle Bin sizes
  cleanup::ICommandRunner* commands = nullptr;  // DISM, run by the service
  std::filesystem::path dism;
  placement::ISystemSpace* system = nullptr;
  placement::IProcessRunner* tools = nullptr;
  const placement::IDriveFacts* drives = nullptr;
  // The owner's environment, registry and running programs: where automatic recipes are now.
  const placement::Tools* owner_tools = nullptr;
  startup::IStartupSystem* startup = nullptr;  // the owner's startup items and the machine's
  indexer::IIndexerScope* indexer = nullptr;   // the Windows Search crawl scope
};

struct ExecutorOptions {
  std::filesystem::path system_root{L"C:\\"};    // the volume the Windows handlers clean
  std::filesystem::path overflow_root{L"L:\\"};  // quarantine for items of a volume < 10 % free
  // A sandbox: every quarantined item goes below this folder instead (console runs and tests).
  std::optional<std::filesystem::path> quarantine_root;
  // False: Windows handlers, DISM and system settings are never applied (console runs).
  bool system_changes = true;
};

// The candidate source of the owner decisions (space.owner_decisions): cold folders (strategy
// "archive") and installed programs to review (strategy "uninstall").
inline constexpr std::string_view owner_decisions_source = "space.owner_decisions";

// The candidate source of the build-output and cache folders Windows Search indexes
// (space.indexed_folders), strategy "index_exclude".
inline constexpr std::string_view indexed_folders_source = "space.indexed_folders";

// Below this share of free space a volume's quarantined items go to the overflow volume, and a
// quarantine already on it is purged by the next scheduled purge.
inline constexpr double low_space_ratio = 0.10;

// The rules a scan evaluates.
struct RuleSet {
  std::span<const rules::CleaningRule> rules;
  const clean::Workspaces* workspaces = nullptr;  // null: no developer-artifact scan
};

struct ScanResult {
  std::int64_t candidates = 0;
  Bytes bytes;
  std::vector<PlanId> plans;          // plans created by this scan
  std::vector<std::string> problems;  // handlers that could not be sized, and the like
};

// What propose_optimization changes.
struct Optimization {
  std::string kind;    // hibernation | page_file | shadow_storage | relocate
  std::string mode;    // hibernation: off | reduced | full; empty: recommended
  std::string target;  // page_file, relocate: drive letter; empty: best healthy volume
  std::string volume;  // shadow_storage: drive letter; empty: the system drive
  std::optional<double> maximum_gb;  // shadow_storage; nullopt: recommended
  std::string cache;                 // relocate: a placement::CacheKind text form
  std::string content;               // relocate: move | regenerate; empty: recommended
  // relocate: a knowledge pack's recipe instead of a cache, and the instance it moves (empty
  // when its setting names none)
  std::optional<packs::RelocationRecipe> recipe;
  std::string instance;
};

class Executor {
 public:
  // Every reference must outlive the executor.
  Executor(const actions::Environment& environment, actions::PolicyEngine& engine,
           actions::Quarantine& quarantine, Probes probes, safety::ProtectedLocations locations,
           ExecutorOptions options);

  // Evaluates `rules`, sizes the system-area handlers and the aged Recycle Bin items, replaces
  // the candidates of source "scan" and records report-tier ones as findings. Then one plan per
  // group that has no open plan yet: ask-tier items by category (quarantine), native commands,
  // handlers by scope, the aged Recycle Bin items, DISM, all waiting for approval; when
  // `with_auto`, also the auto-tier items, approved at once (deleted by the service, or run by
  // the tray for user scope).
  [[nodiscard]] Result<ScanResult> scan(const RuleSet& rules, bool with_auto,
                                        const std::stop_token& stop);

  // The approved service-scope plans, oldest first.
  [[nodiscard]] Result<std::vector<PlanId>> approved_service_plans() const;
  // Runs approved service-scope plan `id`. An auto run of DeleteItems also writes its manifest:
  // an action_log entry "manifest" with the bytes freed, items removed and skipped, per rule.
  [[nodiscard]] Result<actions::RunReport> execute(PlanId id);
  // Undoes applied or verified service-scope plan `id` for the owner; the new state.
  [[nodiscard]] Result<PlanState> undo(PlanId id);

  // A quarantine plan over candidates `ids` (path candidates only), awaiting approval.
  [[nodiscard]] Result<Plan> propose_cleanup(std::span<const std::int64_t> ids,
                                             const Actor& proposer);
  // An approved user-scope plan that sends candidates `ids` to the Recycle Bin (the owner's
  // explicit deletion from a report view).
  [[nodiscard]] Result<Plan> recycle(std::span<const std::int64_t> ids);
  // An approved user-scope plan for owner-decision candidate `id` (owner_decisions_source): a
  // cold folder moved to the HDD archive (action "ArchiveFolder", kind archive, params
  // {"source", "target"}: its scanall::archive_folder on the healthy HDD with the most room that
  // keeps 10 % free, judged now; the item records the folder's identity) or a program
  // uninstalled by its own uninstaller (action "UninstallProgram", kind uninstall, one item
  // "uninstall:<name>", params {"key", "name"}). The owner's click is the approval of exactly
  // that plan. Fails with not_found for an unknown candidate, a folder gone or a program
  // uninstalled since the list was made, invalid_argument for a candidate that is no owner
  // decision, refused for a system or deny-list place, and invalid_state when no healthy HDD
  // keeps 10 % free after taking the folder.
  [[nodiscard]] Result<Plan> owner_action(std::int64_t id);
  // An approval-only plan for a system setting or a relocation (of a cache, or by a recipe: action
  // "RelocateRecipe", one item "relocate:<pack>/<case>[:<instance>]", the target folder
  // "<root>Relocated\<pack>\<case>[\<instance>]" on the best healthy volume, params
  // {"recipe": placement::recipe_json(), "instance", "target", "content"}).
  [[nodiscard]] Result<Plan> propose_optimization(const Optimization& request,
                                                  const Actor& proposer);
  // An approval-only plan that disables (or enables) the startup items `ids` (startup::Item::id):
  // action "SetStartupItems", kind system_setting, one item per id with the id as its path,
  // params {"items", "enable"}. Fails with invalid_state without the startup probe, and as
  // StartupToggle::preview does for an id it cannot change.
  [[nodiscard]] Result<Plan> propose_startup(std::span<const std::string> ids, bool enable,
                                             const Actor& proposer);
  // An approval-only plan that keeps the folders of indexed-folder candidates `ids`
  // (indexed_folders_source) out of the Windows Search index: action "SetIndexerScope", kind
  // system_setting, one item per folder, params {"folders"}. Fails with invalid_state without
  // the indexer probe, not_found for an unknown or stale candidate, invalid_argument for one of
  // another source, and as IndexerExclusion::preview does for a folder no longer indexed.
  [[nodiscard]] Result<Plan> propose_index_exclusion(std::span<const std::int64_t> ids,
                                                     const Actor& proposer);
  // When the system volume has less than low_space_ratio free: automatic plans (approved at once,
  // run by the service, undoable) that reduce full hibernation and that keep a 4096 MB page file
  // on the system volume for crash dumps while a system-managed one goes to the best healthy
  // volume, so the page file never shrinks below the commit in use; and for every automatic
  // recipe of `recipes` whose content is on the system volume and none of whose `needs` runs, an
  // automatic relocation to the best healthy volume, run by the tray (one per instance). Each only
  // when it is recommended and no plan of its own is open or was made in the last 7 days;
  // nothing when system settings are not applied here (console runs). The plans created.
  [[nodiscard]] Result<std::vector<PlanId>> reclaim_system_space(
      std::span<const packs::RelocationRecipe> recipes = {});

  // Moves held items of run `run` back (every held item when `item` is nullopt); how many.
  [[nodiscard]] Result<std::size_t> restore(PlanId run, std::optional<PlanItemId> item);
  // Purges the quarantine runs older than 7 days, and every run's folder on a volume with less
  // than low_space_ratio free (it frees nothing until purged); how many items.
  [[nodiscard]] Result<std::size_t> purge();
  // Purges every held item of run `run` now, for the owner; how many items.
  [[nodiscard]] Result<std::size_t> purge_run(PlanId run);

  // The plan or purge execute(), purge() or purge_run() is applying now; nullopt when none
  // runs. Safe to call from any thread.
  [[nodiscard]] std::optional<Applying> applying() const { return progress_.now(); }

 private:
  // What the rules select now, the system-area handlers and the aged Recycle Bin items sized; a
  // handler or bin that cannot be sized is a problem and no candidate.
  [[nodiscard]] Result<std::vector<Candidate>> evaluate(const RuleSet& rules, WallTime now,
                                                        const std::stop_token& stop,
                                                        std::vector<std::string>& problems) const;
  // Replaces the scan's candidates (filling in their ids) and records the report-tier ones as
  // findings.
  [[nodiscard]] Result<void> record(std::vector<Candidate>& found, WallTime now) const;
  // The plan and spec of owner-decision `candidate` (owner_action).
  [[nodiscard]] Result<std::pair<Plan, PlanSpec>> archive_plan(const Candidate& candidate) const;
  [[nodiscard]] Result<std::pair<Plan, PlanSpec>> uninstall_plan(const Candidate& candidate) const;
  [[nodiscard]] Result<Plan> submit_setting(Action& action, const std::string& name,
                                            const PlanSpec& spec, const Actor& proposer);
  // `spec` as an automatic plan of group `group` from the scheduler, approved at once.
  [[nodiscard]] Result<Plan> submit_automatic(Action& action, const std::string& name,
                                              PlanSpec spec, const std::string& group);
  // reclaim_system_space's plan for hibernation, and for the page file over `volumes`; nullopt
  // when nothing is recommended.
  [[nodiscard]] Result<std::optional<PlanId>> automatic_hibernation();
  [[nodiscard]] Result<std::optional<PlanId>> automatic_page_file(
      std::span<const placement::TargetVolume> volumes);
  [[nodiscard]] Result<Plan> propose_hibernation(const Optimization& request,
                                                 const Actor& proposer);
  [[nodiscard]] Result<Plan> propose_page_file(const Optimization& request,
                                               std::vector<placement::TargetVolume> volumes,
                                               const Actor& proposer);
  [[nodiscard]] Result<Plan> propose_shadow_storage(const Optimization& request,
                                                    std::vector<placement::TargetVolume> volumes,
                                                    const Actor& proposer);
  [[nodiscard]] Result<Plan> propose_relocation(const Optimization& request,
                                                std::vector<placement::TargetVolume> volumes,
                                                const Actor& proposer);
  // An approval-only plan, or with `group` an automatic plan of that group from the scheduler.
  [[nodiscard]] Result<Plan> propose_recipe(const Optimization& request,
                                            std::vector<placement::TargetVolume> volumes,
                                            const Actor& proposer, const std::string& group = {});
  // reclaim_system_space's relocations of the automatic `recipes` on the system volume.
  [[nodiscard]] Result<void> automatic_relocations(std::span<const packs::RelocationRecipe> recipes,
                                                   std::span<const placement::TargetVolume> volumes,
                                                   const std::set<std::string>& open,
                                                   std::vector<PlanId>& plans);
  // The volume a relocation goes to: request.target, or the best healthy one with room.
  [[nodiscard]] Result<placement::TargetVolume> relocation_volume(
      const Optimization& request, std::span<const placement::TargetVolume> volumes) const;
  // The plan items of path candidates `ids` (identity read now), with their notes.
  [[nodiscard]] Result<std::vector<PlanItem>> items_of(std::span<const std::int64_t> ids,
                                                       std::vector<ItemNote>& notes) const;
  // The groups of the plans not finished yet (proposed .. applying), and with `since` of every
  // plan created since then.
  [[nodiscard]] Result<std::set<std::string>> open_groups(
      std::optional<WallTime> since = std::nullopt) const;
  [[nodiscard]] Result<std::unique_ptr<Action>> action_for(const Plan& plan, const PlanSpec& spec,
                                                           const Actor& actor) const;
  // Where QuarantineItems keeps each item, judged from the volumes as they are now.
  [[nodiscard]] actions::QuarantineRoot quarantine_root() const;
  [[nodiscard]] Result<void> write_manifest(PlanId id) const;

  actions::Environment environment_;
  actions::PolicyEngine* engine_;
  actions::Quarantine* quarantine_;
  Probes probes_;
  safety::ProtectedLocations locations_;
  ExecutorOptions options_;
  mutable ApplyTracker progress_;  // told by the actions it builds; synchronized
};

}  // namespace myclean::cleaning
