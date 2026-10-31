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
#include <vector>

#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleanup/command_runner.hpp"
#include "core/cleanup/handler_host.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"

// The cleaning executor (cleaning spec C4): turns what the rules select into plans by tier and
// scope, and runs the approved service-scope plans through the action framework.
namespace myclean::cleaning {

// The machine as the executor sees it. Every pointer must outlive the executor; a null one
// disables what needs it (no git: no developer artifacts; no handlers: no handler sizes or
// purges; no drives: every item is quarantined on its own volume).
struct Probes {
  const clean::IFileTree* tree = nullptr;
  const clean::IEnvironment* environment = nullptr;  // the owner's variables
  const clean::IUsage* usage = nullptr;
  const clean::IGit* git = nullptr;
  cleanup::IHandlerHost* handlers = nullptr;    // system-area handlers, run by the service
  cleanup::ICommandRunner* commands = nullptr;  // DISM, run by the service
  std::filesystem::path dism;
  placement::ISystemSpace* system = nullptr;
  placement::IProcessRunner* tools = nullptr;
  const placement::IDriveFacts* drives = nullptr;
};

struct ExecutorOptions {
  std::filesystem::path system_root{L"C:\\"};    // the volume the Windows handlers clean
  std::filesystem::path overflow_root{L"L:\\"};  // quarantine for items of a volume < 10 % free
  // A sandbox: every quarantined item goes below this folder instead (console runs and tests).
  std::optional<std::filesystem::path> quarantine_root;
  // False: Windows handlers, DISM and system settings are never applied (console runs).
  bool system_changes = true;
};

// Below this share of free space a volume's quarantined items go to the overflow volume.
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

  // Evaluates `rules`, sizes the system-area handlers, replaces the candidates of source "scan"
  // and records report-tier ones as findings. Then one plan per group that has no open plan
  // yet: ask-tier items by category (quarantine), native commands, handlers by scope, DISM,
  // all waiting for approval; when `scheduled`, also the auto-tier items, approved at once
  // (deleted by the service, or run by the tray for user scope).
  [[nodiscard]] Result<ScanResult> scan(const RuleSet& rules, bool scheduled,
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
  // An approval-only plan for a system setting or a relocation (of a cache, or by a recipe: action
  // "RelocateRecipe", one item "relocate:<pack>/<case>[:<instance>]", the target folder
  // "<root>Relocated\<pack>\<case>[\<instance>]" on the best healthy volume, params
  // {"recipe": placement::recipe_json(), "instance", "target", "content"}).
  [[nodiscard]] Result<Plan> propose_optimization(const Optimization& request,
                                                  const Actor& proposer);

  // Moves held items of run `run` back (every held item when `item` is nullopt); how many.
  [[nodiscard]] Result<std::size_t> restore(PlanId run, std::optional<PlanItemId> item);
  // Purges the quarantine runs older than 7 days; how many items.
  [[nodiscard]] Result<std::size_t> purge();

 private:
  // What the rules select now, the system-area handlers sized; a handler that cannot be sized
  // is a problem and no candidate.
  [[nodiscard]] Result<std::vector<Candidate>> evaluate(const RuleSet& rules, WallTime now,
                                                        const std::stop_token& stop,
                                                        std::vector<std::string>& problems) const;
  // Replaces the scan's candidates (filling in their ids) and records the report-tier ones as
  // findings.
  [[nodiscard]] Result<void> record(std::vector<Candidate>& found, WallTime now) const;
  [[nodiscard]] Result<Plan> submit_setting(Action& action, const std::string& name,
                                            const PlanSpec& spec, const Actor& proposer);
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
  [[nodiscard]] Result<Plan> propose_recipe(const Optimization& request,
                                            std::vector<placement::TargetVolume> volumes,
                                            const Actor& proposer);
  // The volume a relocation goes to: request.target, or the best healthy one with room.
  [[nodiscard]] Result<placement::TargetVolume> relocation_volume(
      const Optimization& request, std::span<const placement::TargetVolume> volumes) const;
  // The plan items of path candidates `ids` (identity read now), with their notes.
  [[nodiscard]] Result<std::vector<PlanItem>> items_of(std::span<const std::int64_t> ids,
                                                       std::vector<ItemNote>& notes) const;
  // The groups of the plans not finished yet (proposed .. applying).
  [[nodiscard]] Result<std::set<std::string>> open_groups() const;
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
};

}  // namespace myclean::cleaning
