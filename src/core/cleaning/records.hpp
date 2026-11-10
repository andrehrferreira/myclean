#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/policy.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// What the cleaning executor stores besides the plans themselves: each plan's spec (kind, tier,
// scope, what it runs and the action parameters, in plan_params), why each item is in its plan,
// and the candidates scans and analyses found, addressable by id for proposals.
namespace myclean::cleaning {

// Tables cleaning_candidate and cleaning_item (component "cleaning").
[[nodiscard]] const store::MigrationSet& cleaning_migrations();

// recycle_bin: the Recycle Bin items deleted long ago, removed by the tray. archive: a cold folder
// moved to the HDD archive, uninstall: a program's own uninstaller, both the owner's one-click
// decisions run by the tray. compact: WSL and Docker virtual disks compacted (not reversible).
// pause: a freeze suspect paused for a while (guided freeze elimination).
enum class PlanKind {
  cleanup,
  handler,
  native,
  relocate,
  system_setting,
  quarantine_restore,
  recycle_bin,
  archive,
  uninstall,
  compact,
  pause
};

[[nodiscard]] std::string_view to_string(PlanKind kind);
[[nodiscard]] std::optional<PlanKind> parse_plan_kind(std::string_view text);

// "user" or "service" (the IPC form of ExecutionContext).
[[nodiscard]] std::string_view scope_name(ExecutionContext scope);

// Everything the views and the executor need besides the stored plan, kept as JSON in
// plan_params so that the executor rebuilds exactly the action the owner approved.
struct PlanSpec {
  PlanKind kind = PlanKind::cleanup;
  Tier tier = Tier::ask;
  ExecutionContext scope = ExecutionContext::service;
  std::string title;
  std::string group;  // the scan group a scheduled scan does not repeat while it is open
  bool needs_restart = false;
  std::vector<std::string> commands;  // exactly what runs
  std::string undo;                   // how it is undone, or why it cannot be
  std::vector<std::string> warnings;
  nlohmann::json params = nlohmann::json::object();  // the action's own parameters
};

[[nodiscard]] std::string to_text(const PlanSpec& spec);
// Fails with invalid_data on malformed text.
[[nodiscard]] Result<PlanSpec> parse_spec(std::string_view text);
// The spec of a plan of action `action` stored without one (the quarantine's own restore and
// purge plans).
[[nodiscard]] PlanSpec implicit_spec(std::string_view action);
// The stored spec of `plan`, or implicit_spec() when it has none.
[[nodiscard]] Result<PlanSpec> spec_of(store::Database& db, const Plan& plan);

// Why one item is in its plan.
struct ItemNote {
  std::string rule;  // the rule id; empty when no rule selected it
  std::string reason;
};

// Stores `plan` with `spec` and the note of each item (by position), moving it on by `level`
// (PolicyEngine::submit).
[[nodiscard]] Result<Plan> submit(actions::PolicyEngine& engine, store::Store& store, Plan plan,
                                  const PlanSpec& spec, std::span<const ItemNote> notes,
                                  actions::AutomationLevel level);
// The note of item `item`; nullopt when it has none.
[[nodiscard]] Result<std::optional<ItemNote>> note_of(store::Database& db, PlanItemId item);

// One thing a scan or an analysis found.
struct Candidate {
  std::int64_t id = 0;  // AUTOINCREMENT: an id of a replaced candidate is never reused
  std::string source;   // "scan", "space.top:C", "space.old_downloads", ...
  std::string rule;     // the rule id; empty for analyses
  std::string category;
  Tier tier = Tier::report;
  ExecutionContext scope = ExecutionContext::service;
  std::string strategy;        // rules::Strategy text form; "file" for analyses
  std::filesystem::path path;  // empty for native and handler candidates
  Bytes size;
  std::string command;  // native; uninstall: the program's Uninstall entry (its registry key)
  std::string handler;  // handler; uninstall: the program's DisplayName
  std::string reason;
  WallTime found_at;
};

// Replaces every candidate of `source` with `candidates`, filling in their ids.
[[nodiscard]] Result<void> replace_candidates(store::Database& db, std::string_view source,
                                              std::vector<Candidate>& candidates);
// The candidates with `ids`, in that order. Fails with not_found naming an unknown id.
[[nodiscard]] Result<std::vector<Candidate>> candidates_by_id(store::Database& db,
                                                              std::span<const std::int64_t> ids);
// Count and total size of the candidates of `source`.
struct CandidateTotals {
  std::int64_t count = 0;
  Bytes bytes;
};
[[nodiscard]] Result<CandidateTotals> candidate_totals(store::Database& db,
                                                       std::string_view source);

}  // namespace myclean::cleaning
