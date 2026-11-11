#include "core/cleaning/executor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/audit_log.hpp"
#include "core/actions/delete_items.hpp"
#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/alerts/engine.hpp"
#include "core/apps/apps.hpp"
#include "core/clean/engine.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleanup/dism_cleanup.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/elimination/pause_action.hpp"
#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/indexer/exclusion.hpp"
#include "core/input/incident_store.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/proposal.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/placement/system_space.hpp"
#include "core/resident/history.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/owner_decisions.hpp"
#include "core/startup/boot_times.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"
#include "core/startup/toggle.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"
#include "core/vhdx/compact.hpp"

namespace myclean::cleaning {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

// True when `volume` (known) has less than low_space_ratio of its size free.
bool low_on_space(const placement::TargetVolume* volume) {
  return volume != nullptr && volume->size.count() != 0 &&
         static_cast<double>(volume->free.count()) <
             low_space_ratio * static_cast<double>(volume->size.count());
}

constexpr std::string_view scan_source = "scan";
// The groups of reclaim_system_space's automatic plans.
constexpr std::string_view hibernation_group = "auto:hibernation";
constexpr std::string_view page_file_group = "auto:page-file";

std::string utf8(const fs::path& path) {
  return utf8_text(path);
}

fs::path from_utf8(const std::string& text) {
  return {std::u8string(text.begin(), text.end())};
}

// `stored` with every item approved by the owner, whose click was the approval.
Result<Plan> approved_by_owner(actions::PolicyEngine& engine, Result<Plan> stored) {
  if (!stored) {
    return stored;
  }
  std::vector<PlanItemId> all;
  for (const PlanItem& item : stored->items) {
    all.push_back(item.id);
  }
  Result<PlanState> approved = engine.approve(stored->id, all, Actor::owner());
  if (!approved) {
    return tl::unexpected(approved.error());
  }
  stored->state = *approved;
  return stored;
}

// Why a rule selected `candidate`, for the owner.
std::string reason_of(const clean::Candidate& candidate, WallTime now) {
  std::string reason = candidate.category;
  if (candidate.newest != WallTime{}) {
    const auto days = std::chrono::floor<std::chrono::days>(now - candidate.newest).count();
    reason += ", unchanged for " + std::to_string(days) + " days";
  }
  if (!candidate.project.empty()) {
    reason += ", inactive project " + utf8(candidate.project);
  }
  if (candidate.strategy == rules::Strategy::native) {
    reason += ", pruned by `" + candidate.command + "`";
  } else if (candidate.strategy == rules::Strategy::handler) {
    reason += ", Windows handler \"" + candidate.handler + "\"";
  }
  return reason;
}

// The deletion age of the aged Recycle Bin case that selected `candidate`: a user-scope
// "Recycle Bin" handler case with require.min_age_days; nullopt for any other candidate.
std::optional<int> bin_age(std::span<const rules::CleaningRule> rules,
                           const clean::Candidate& candidate) {
  if (candidate.strategy != rules::Strategy::handler ||
      candidate.handler != cleanup::recycle_bin_handler ||
      candidate.scope != ExecutionContext::user) {
    return std::nullopt;
  }
  const auto rule = std::ranges::find(rules, candidate.rule_id, &rules::CleaningRule::id);
  return rule == rules.end() ? std::nullopt : rule->require.min_age_days;
}

// Sizes `candidate`, the aged Recycle Bin case of `days`, from the owner's $I records and sets its
// item and `reason`; false when it is no candidate: nothing deleted that long ago, or (a problem)
// too young an age or a bin that cannot be read.
bool size_aged_bin(const cleanup::IBinRecords& bin, int days, WallTime now,
                   const std::stop_token& stop, clean::Candidate& candidate, std::string& reason,
                   std::vector<std::string>& problems) {
  if (days < cleanup::min_bin_days) {
    problems.push_back("Recycle Bin: " + candidate.rule_id + " asks for items deleted " +
                       std::to_string(days) + " days ago; at least " +
                       std::to_string(cleanup::min_bin_days) + " are required");
    return false;
  }
  const Result<std::vector<cleanup::BinFile>> files = bin.read(stop);
  if (!files) {
    problems.push_back("Recycle Bin: " + files.error().message);
    return false;
  }
  const cleanup::AgedItems aged = cleanup::aged_records(*files, now, std::chrono::days{days});
  if (aged.bytes.count() == 0) {
    return false;
  }
  candidate.size = aged.bytes;
  candidate.path = cleanup::recycle_bin_item(days);
  reason = candidate.category + ", " + std::to_string(aged.count) + " items deleted " +
           std::to_string(days) + " days ago or earlier";
  return true;
}

// One plan a scan builds.
struct Group {
  Plan plan;
  PlanSpec spec;
  std::vector<ItemNote> notes;
  actions::AutomationLevel level = actions::AutomationLevel::suggest;
};

// "C" -> "C:\"; fails for anything but one letter.
Result<fs::path> drive_root_of(std::string_view letter) {
  if (letter.size() != 1 || std::isalpha(static_cast<unsigned char>(letter[0])) == 0) {
    return fail(Errc::invalid_argument, "a drive is one letter, such as \"D\"");
  }
  const auto upper = static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(letter[0])));
  return fs::path(std::wstring{upper, L':', L'\\'});
}

// "C:" for "C:\".
std::string drive_name(const fs::path& root) {
  const std::string text = utf8(root);
  return text.substr(0, 2);
}

Json page_file_json(const placement::PageFileConfig& config) {
  Json files = Json::array();
  for (const placement::PageFile& file : config.files) {
    files.push_back(Json{{"path", utf8(file.path)},
                         {"initial_mb", file.initial_mb},
                         {"maximum_mb", file.maximum_mb}});
  }
  return Json{{"automatic", config.automatic}, {"files", std::move(files)}};
}

Result<placement::PageFileConfig> page_file_config(const Json& json) {
  try {
    placement::PageFileConfig config{.automatic = json.at("automatic").get<bool>(), .files = {}};
    for (const Json& file : json.at("files")) {
      config.files.push_back(
          placement::PageFile{.path = from_utf8(file.at("path").get<std::string>()),
                              .initial_mb = file.at("initial_mb").get<std::uint32_t>(),
                              .maximum_mb = file.at("maximum_mb").get<std::uint32_t>()});
    }
    return config;
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, std::string("page-file parameters: ") + error.what());
  }
}

PlanSpec setting_spec(const placement::SettingSummary& summary, std::string title, Json params) {
  std::string undo;
  for (const std::string& command : summary.undo_commands) {
    undo += (undo.empty() ? "" : "; ") + command;
  }
  return PlanSpec{.kind = PlanKind::system_setting,
                  .tier = Tier::ask,
                  .scope = ExecutionContext::service,
                  .title = std::move(title) + ": " + summary.current + " -> " + summary.proposed,
                  .needs_restart = summary.restart_required,
                  .commands = summary.commands,
                  .undo = undo.empty() ? "restores the previous setting" : undo,
                  .warnings = {},
                  .params = std::move(params)};
}

// The bytes an "apply" log entry records as freed.
std::uint64_t freed_of(const actions::LogEntry& entry) {
  constexpr std::string_view prefix = "freed=";
  return entry.metrics.starts_with(prefix) ? std::stoull(entry.metrics.substr(prefix.size())) : 0;
}

// The rule that selected plan item `item`, for the manifest.
std::string rule_of(store::Database& db, PlanItemId item) {
  const Result<std::optional<ItemNote>> note = note_of(db, item);
  const std::optional<ItemNote> known = note ? *note : std::nullopt;
  return known ? known->rule : std::string("(no rule)");
}

// Where one candidate goes: its group, the plan's action and spec, and its item.
struct Slot {
  std::string key;
  std::string action;
  PlanSpec spec;
  PlanItem item;
};

Slot slot_of(const Candidate& candidate, const fs::path& system_root) {
  const std::string tier(to_string(candidate.tier));
  const bool automatic = candidate.tier == Tier::automatic;
  Slot slot{.spec = PlanSpec{.tier = candidate.tier, .scope = ExecutionContext::service},
            .item = PlanItem{.path = candidate.path, .size = candidate.size}};
  PlanSpec& spec = slot.spec;
  const std::optional<int> bin_days =
      candidate.strategy == "handler" ? cleanup::recycle_bin_days(candidate.path) : std::nullopt;
  if (candidate.strategy == "native" && candidate.scope == ExecutionContext::service) {
    slot.key = tier + ":dism";
    slot.action = "DismCleanup";
    slot.item.path = cleanup::component_store_item();
    spec.kind = PlanKind::handler;
    spec.title = "Clean up the Windows component store";
    spec.commands = {candidate.command};
    spec.undo = "not reversible: superseded components are removed";
  } else if (candidate.strategy == "native") {
    slot.key = tier + ":native:" + candidate.command;
    slot.action = "NativeCommand";
    slot.item.path = from_utf8("native:" + candidate.command);
    spec.kind = PlanKind::native;
    spec.scope = ExecutionContext::user;
    spec.title = "Run `" + candidate.command + "`";
    spec.commands = {candidate.command};
    spec.undo = "not reversible: the tool downloads what it needs again";
    spec.params = Json{{"command", candidate.command}};
  } else if (bin_days) {
    const std::string days = std::to_string(*bin_days);
    slot.key = tier + ":recycle-bin";
    slot.action = "RecycleBinCleanup";
    spec.kind = PlanKind::recycle_bin;
    spec.scope = ExecutionContext::user;
    spec.title = "Empty the Recycle Bin of items deleted " + days + " days ago or earlier";
    spec.commands = {
        "IFileOperation permanent delete with FOF_NO_UI of the Recycle Bin items of "
        "every fixed volume deleted at least " +
        days + " days ago"};
    spec.undo = "not reversible: the items are deleted permanently";
    spec.params = Json{{"deleted_days", *bin_days}};
  } else if (candidate.strategy == "handler") {
    slot.key = tier + ":handler:" + std::string(scope_name(candidate.scope));
    slot.action = "HandlerCleanup";
    slot.item.path = cleanup::handler_item(candidate.handler);
    spec.kind = PlanKind::handler;
    spec.scope = candidate.scope;
    spec.title = "Windows disk cleanup";
    spec.undo = "not reversible: Windows regenerates what it needs";
    spec.params = Json{{"volume", utf8(system_root)}, {"handlers", Json::array()}};
  } else {
    slot.key = automatic ? "auto:delete" : "ask:" + candidate.category;
    slot.action = automatic ? "DeleteItems" : "QuarantineItems";
    spec.title = automatic ? "Delete regenerable files" : "Quarantine " + candidate.category;
    spec.undo = automatic ? "not reversible: the files are rebuilt when needed"
                          : "restore from the quarantine until it is purged after 7 days";
  }
  spec.group = slot.key;
  return slot;
}

// The plans of a scan, by group: ask-tier and native/handler candidates always, auto-tier ones
// when `with_auto`; never report-tier ones. A file item gets the identity it has now;
// one that is gone since the scan is left out.
std::map<std::string, Group> groups_of(const std::vector<Candidate>& found, bool with_auto,
                                       WallTime now, const fs::path& system_root,
                                       const os::IFileSystem& file_system) {
  std::map<std::string, Group> groups;
  for (const Candidate& candidate : found) {
    if (candidate.tier == Tier::report || (candidate.tier == Tier::automatic && !with_auto)) {
      continue;
    }
    Slot slot = slot_of(candidate, system_root);
    if (slot.action == "DeleteItems" || slot.action == "QuarantineItems") {
      const Result<os::FileStat> status = file_system.stat(candidate.path);
      if (!status) {
        continue;
      }
      slot.item.identity = status->identity;
    }
    auto entry = groups.find(slot.key);
    if (entry == groups.end()) {
      const auto level = candidate.tier == Tier::automatic ? actions::AutomationLevel::automatic
                                                           : actions::AutomationLevel::suggest;
      entry = groups
                  .emplace(slot.key, Group{.plan = Plan{.action = slot.action,
                                                        .proposer = Actor::scheduler(),
                                                        .created_at = now},
                                           .spec = std::move(slot.spec),
                                           .notes = {},
                                           .level = level})
                  .first;
    }
    Group& group = entry->second;
    if (slot.action == "HandlerCleanup") {
      group.spec.params["handlers"].push_back(candidate.handler);
      group.spec.commands.push_back("Windows disk cleanup handler \"" + candidate.handler +
                                    "\" on " + utf8(system_root));
    }
    group.plan.items.push_back(std::move(slot.item));
    group.notes.push_back(ItemNote{.rule = candidate.rule, .reason = candidate.reason});
  }
  for (auto& [key, group] : groups) {
    const std::string count = std::to_string(group.plan.items.size());
    if (group.plan.action == "DeleteItems") {
      group.spec.commands = {"delete " + count + " items permanently"};
    } else if (group.plan.action == "QuarantineItems") {
      group.spec.commands = {"move " + count + " items to the quarantine for 7 days"};
    }
  }
  return groups;
}

}  // namespace

Executor::Executor(const actions::Environment& environment, actions::PolicyEngine& engine,
                   actions::Quarantine& quarantine, Probes probes,
                   safety::ProtectedLocations locations, ExecutorOptions options)
    : environment_(environment),
      engine_(&engine),
      quarantine_(&quarantine),
      probes_(std::move(probes)),
      locations_(std::move(locations)),
      options_(std::move(options)) {}

Result<std::set<std::string>> Executor::open_groups(std::optional<WallTime> since) const {
  std::set<std::string> groups;
  const Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(
        "SELECT pp.params FROM plan p JOIN plan_params pp ON pp.plan_id = p.id "
        "WHERE p.state IN ('proposed', 'auto', 'awaiting_approval', 'approved', 'applying') "
        "OR p.created_at >= ?1");
    if (!query) {
      return tl::unexpected(query.error());
    }
    query->bind(1,
                since ? store::to_unix_seconds(*since) : std::numeric_limits<std::int64_t>::max());
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      if (Result<PlanSpec> spec = parse_spec(query->column_text(0)); spec && !spec->group.empty()) {
        groups.insert(spec->group);
      }
    }
    return row ? Result<void>{} : tl::unexpected(row.error());
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return groups;
}

Result<std::vector<Candidate>> Executor::evaluate(const RuleSet& rules, WallTime now,
                                                  const std::stop_token& stop,
                                                  std::vector<std::string>& problems) const {
  if (probes_.tree == nullptr || probes_.environment == nullptr || probes_.usage == nullptr) {
    return fail(Errc::invalid_state, "the file tree, environment and usage probes are required");
  }
  const clean::RuleEngine engine(*probes_.tree, *probes_.environment, *probes_.usage, locations_);
  clean::Evaluation evaluation = engine.evaluate(rules.rules, now);
  if (probes_.git != nullptr && rules.workspaces != nullptr) {
    clean::Evaluation artifacts =
        engine.scan_artifacts(rules.rules, *rules.workspaces, *probes_.git, now);
    std::ranges::move(artifacts.candidates, std::back_inserter(evaluation.candidates));
  }
  std::vector<Candidate> found;
  for (clean::Candidate& candidate : evaluation.candidates) {
    std::string reason = reason_of(candidate, now);
    if (const std::optional<int> days = bin_age(rules.rules, candidate)) {
      // The aged Recycle Bin items, sized from the owner's $I records: none, no candidate.
      if (probes_.bin == nullptr ||
          !size_aged_bin(*probes_.bin, *days, now, stop, candidate, reason, problems)) {
        continue;
      }
    } else if (candidate.strategy == rules::Strategy::handler &&
               candidate.scope == ExecutionContext::service) {
      // The service sizes the system-area handlers; one without a host, or with nothing to
      // free, is no candidate.
      if (probes_.handlers == nullptr) {
        continue;
      }
      const Result<Bytes> size =
          probes_.handlers->space_used(candidate.handler, options_.system_root, stop);
      if (!size) {
        problems.push_back("handler \"" + candidate.handler + "\": " + size.error().message);
      }
      if (!size || size->count() == 0) {
        continue;
      }
      candidate.size = *size;
    }
    found.push_back(Candidate{.rule = candidate.rule_id,
                              .category = candidate.category,
                              .tier = candidate.tier,
                              .scope = candidate.scope,
                              .strategy = std::string(rules::to_string(candidate.strategy)),
                              .path = candidate.path,
                              .size = candidate.size,
                              .command = candidate.command,
                              .handler = candidate.handler,
                              .reason = std::move(reason),
                              .found_at = now});
  }
  return found;
}

Result<void> Executor::record(std::vector<Candidate>& found, WallTime now) const {
  // Report-tier items are findings only: one per category.
  std::map<std::string, std::pair<std::int64_t, Bytes>> reported;
  for (const Candidate& candidate : found) {
    if (candidate.tier == Tier::report) {
      auto& [count, bytes] = reported[candidate.category];
      ++count;
      bytes += candidate.size;
    }
  }
  return environment_.store->write([&](store::Database& db) -> Result<void> {
    if (Result<void> replaced = replace_candidates(db, scan_source, found); !replaced) {
      return replaced;
    }
    for (const auto& [category, totals] : reported) {
      const Finding finding{.kind = "clean.report",
                            .subject = category,
                            .summary = std::to_string(totals.first) + " items, " +
                                       std::to_string(totals.second.count()) +
                                       " bytes, shown only (tier report)",
                            .detected_at = now};
      if (Result<FindingId> added = store::add_finding(db, finding); !added) {
        return tl::unexpected(added.error());
      }
    }
    return {};
  });
}

Result<ScanResult> Executor::scan(const RuleSet& rules, bool with_auto,
                                  const std::stop_token& stop) {
  const WallTime now = environment_.clock->now();
  ScanResult result;
  Result<std::vector<Candidate>> found = evaluate(rules, now, stop, result.problems);
  if (!found) {
    return tl::unexpected(found.error());
  }
  for (const Candidate& candidate : *found) {
    result.bytes += candidate.size;
  }
  result.candidates = static_cast<std::int64_t>(found->size());
  if (Result<void> recorded = record(*found, now); !recorded) {
    return tl::unexpected(recorded.error());
  }
  const Result<std::set<std::string>> open = open_groups();
  if (!open) {
    return tl::unexpected(open.error());
  }
  for (auto& [key, group] :
       groups_of(*found, with_auto, now, options_.system_root, *environment_.file_system)) {
    if (open->contains(key)) {
      continue;
    }
    Result<Plan> stored = submit(*engine_, *environment_.store, std::move(group.plan), group.spec,
                                 group.notes, group.level);
    if (!stored) {
      return tl::unexpected(stored.error());
    }
    result.plans.push_back(stored->id);
  }
  return result;
}

Result<std::vector<PlanId>> Executor::approved_service_plans() const {
  std::vector<PlanId> plans;
  bool paused = false;
  if (probes_.pauses != nullptr) {
    const Result<std::optional<elimination::PauseRecord>> active = probes_.pauses->active();
    paused = !active || active->has_value();  // unknown counts as active
  }
  const Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query =
        db.prepare("SELECT id FROM plan WHERE state = 'approved' ORDER BY id");
    if (!query) {
      return tl::unexpected(query.error());
    }
    std::vector<PlanId> approved;
    Result<bool> row = query->step();
    for (; row && *row; row = query->step()) {
      approved.emplace_back(query->column_int(0));
    }
    if (!row) {
      return tl::unexpected(row.error());
    }
    for (const PlanId id : approved) {
      Result<Plan> plan = store::get_plan(db, id);
      if (!plan) {
        return tl::unexpected(plan.error());
      }
      Result<PlanSpec> spec = spec_of(db, *plan);
      if (spec && spec->scope == ExecutionContext::service) {
        if (plan->action == elimination::pause_action_name && paused) {
          continue;  // one suspect at a time: it starts once the active pause is over
        }
        plans.push_back(id);
      }
    }
    return {};
  });
  if (!read) {
    return tl::unexpected(read.error());
  }
  return plans;
}

actions::QuarantineRoot Executor::quarantine_root() const {
  if (options_.quarantine_root) {
    return [root = *options_.quarantine_root](
               const PlanItem&) -> tl::expected<fs::path, std::string> { return root; };
  }
  if (probes_.drives == nullptr) {
    return {};
  }
  Result<std::vector<placement::TargetVolume>> volumes = probes_.drives->volumes();
  if (!volumes) {
    // Free space unknown: an item's own volume may be the full one, so nothing is quarantined
    // (each item is skipped with this reason and stays where it is) until the drives are read.
    std::string why = "free space of the volumes unknown: " + volumes.error().message;
    return [why = std::move(why)](const PlanItem&) -> tl::expected<fs::path, std::string> {
      return tl::unexpected(why);
    };
  }
  return [volumes = *std::move(volumes), overflow = options_.overflow_root](
             const PlanItem& item) -> tl::expected<fs::path, std::string> {
    const placement::TargetVolume* source = placement::volume_of(volumes, item.path);
    if (!low_on_space(source)) {
      return fs::path{};
    }
    const placement::TargetVolume* target = placement::volume_of(volumes, overflow);
    if (target == source) {
      return fs::path{};
    }
    if (target == nullptr) {
      return tl::unexpected<std::string>("quarantine volume unavailable");
    }
    if (!target->healthy) {
      return tl::unexpected<std::string>("quarantine volume health degraded");
    }
    if (const std::string why = placement::unsuitable(*target, item.size); !why.empty()) {
      return tl::unexpected("quarantine volume: " + why);
    }
    return target->root;
  };
}

namespace {

// The IndexerExclusion of a stored plan's params {"folders"}.
Result<std::unique_ptr<Action>> index_exclusion(indexer::IIndexerScope* scope, const Json& params,
                                                const actions::Environment& environment) {
  if (scope == nullptr) {
    return fail(Errc::invalid_state, "SetIndexerScope needs the Windows Search scope");
  }
  std::vector<fs::path> folders;
  for (const std::string& folder : params.at("folders").get<std::vector<std::string>>()) {
    folders.push_back(from_utf8(folder));
  }
  // The indexer's use from the resident-process history (resident_hour).
  indexer::IndexerLoad load = [store = environment.store](
                                  WallTime from, WallTime to) -> Result<indexer::IndexerUse> {
    Result<resident::Cost> cost = resident::cost_of(*store, indexer::indexer_program, from, to);
    if (!cost) {
      return tl::unexpected(cost.error());
    }
    return indexer::IndexerUse{.cpu_seconds = cost->cpu_seconds, .running = cost->running};
  };
  return std::make_unique<indexer::IndexerExclusion>(*scope, std::move(folders), std::move(load),
                                                     environment.clock);
}

}  // namespace

namespace {

// The StartupToggle of a stored plan's params {"items", "enable"}, verified with the boots stored
// in boot_time.
Result<std::unique_ptr<Action>> startup_toggle(startup::IStartupSystem* system, const Json& params,
                                               const actions::Environment& environment) {
  if (system == nullptr) {
    return fail(Errc::invalid_state, "SetStartupItems needs the startup items");
  }
  return std::make_unique<startup::StartupToggle>(
      *system, *environment.clock, params.at("items").get<std::vector<std::string>>(),
      params.at("enable").get<bool>(),
      [store = environment.store] { return startup::stored_boots(*store); });
}

// The PauseSuspect of stored plan `plan` with params {"image", "mode", "hours"}.
Result<std::unique_ptr<Action>> suspect_pause(elimination::IPauses* pauses, PlanId plan,
                                              const Json& params) {
  if (pauses == nullptr) {
    return fail(Errc::invalid_state, "PauseSuspect needs the pauses of guided freeze elimination");
  }
  const std::optional<elimination::PauseMode> mode =
      elimination::parse_pause_mode(params.at("mode").get<std::string>());
  if (!mode) {
    return fail(Errc::invalid_data, "unknown pause mode");
  }
  return std::make_unique<elimination::PauseSuspect>(
      *pauses, plan, params.at("image").get<std::string>(), *mode,
      std::chrono::hours{params.at("hours").get<std::int64_t>()});
}

// The CompactVhdx of a stored plan's params {"disks"}.
Result<std::unique_ptr<Action>> compaction(const os::IFileSystem& file_system, const Probes& probes,
                                           const Json& params) {
  if (probes.disks == nullptr || probes.wsl == nullptr) {
    return fail(Errc::invalid_state, "CompactVhdx needs the virtual disk compactor and WSL");
  }
  std::vector<fs::path> disks;
  for (const std::string& disk : params.at("disks").get<std::vector<std::string>>()) {
    disks.push_back(from_utf8(disk));
  }
  return std::make_unique<vhdx::CompactVhdx>(file_system, *probes.wsl, *probes.disks,
                                             std::move(disks));
}

}  // namespace

bool optimization(PlanKind kind) {
  return kind == PlanKind::system_setting || kind == PlanKind::compact || kind == PlanKind::pause;
}

std::vector<alerts::Reading> verification_readings(PlanId id, const PlanSpec& spec,
                                                   const actions::RunReport& report) {
  const std::string entity = std::to_string(id.value());
  const auto measured = [&](const Verification& verification) {
    return std::format("{}: {} was {} before the change and {} after it", spec.title,
                       verification.metric, verification.before, verification.after);
  };
  if (report.state == PlanState::rolled_back && report.verification) {
    return {alerts::Reading{.metric = std::string(rolled_back_metric),
                            .entity = entity,
                            .value = 1.0,
                            .detail = measured(*report.verification) + ", so it was undone"}};
  }
  if (!report.alert) {
    return {};
  }
  if (report.verification) {
    return {alerts::Reading{
        .metric = std::string(worse_metric),
        .entity = entity,
        .value = 1.0,
        .detail =
            measured(*report.verification) +
            (report.undo.reversible() ? "; the change was kept and can be undone in the inbox"
                                      : "; it cannot be undone: " + report.undo.not_reversible)}};
  }
  if (!optimization(spec.kind)) {
    return {};
  }
  return {alerts::Reading{
      .metric = std::string(unverified_metric),
      .entity = entity,
      .value = 1.0,
      .detail = spec.title + ": its effect could not be measured: " + report.unverified}};
}

Result<std::unique_ptr<Action>> Executor::action_for(const Plan& plan, const PlanSpec& spec,
                                                     const Actor& actor) const {
  const auto missing = [&](std::string_view what) {
    return fail(Errc::invalid_state, plan.action + " needs " + std::string(what));
  };
  const Json& params = spec.params;
  try {
    if (plan.action == "DeleteItems") {
      auto action = std::make_unique<actions::DeleteItems>(environment_, plan.id, actor);
      action->observe(&progress_);
      return action;
    }
    if (plan.action == "QuarantineItems") {
      auto action = std::make_unique<actions::QuarantineItems>(environment_, *quarantine_, plan.id,
                                                               actor, quarantine_root());
      action->observe(&progress_);
      return action;
    }
    if (plan.action == "HandlerCleanup") {
      if (probes_.handlers == nullptr) {
        return missing("the handler host");
      }
      return std::make_unique<cleanup::HandlerCleanup>(
          *probes_.handlers, from_utf8(params.at("volume").get<std::string>()),
          params.at("handlers").get<std::vector<std::string>>(), spec.tier,
          ExecutionContext::service);
    }
    if (plan.action == "DismCleanup") {
      if (probes_.commands == nullptr) {
        return missing("the command runner");
      }
      return std::make_unique<cleanup::DismCleanup>(*probes_.commands, probes_.dism, spec.tier);
    }
    if (plan.action == indexer::exclusion_action_name) {
      return index_exclusion(probes_.indexer, params, environment_);
    }
    if (plan.action == startup::toggle_action_name) {
      return startup_toggle(probes_.startup, params, environment_);
    }
    if (plan.action == vhdx::compact_action_name) {
      return compaction(*environment_.file_system, probes_, params);
    }
    if (plan.action == elimination::pause_action_name) {
      return suspect_pause(probes_.pauses, plan.id, params);
    }
    if (probes_.system == nullptr || probes_.tools == nullptr || probes_.drives == nullptr) {
      return missing("the system settings");
    }
    if (plan.action == "SetHibernation") {
      const std::optional<placement::HibernationMode> mode =
          placement::parse_hibernation_mode(params.at("mode").get<std::string>());
      if (!mode) {
        return fail(Errc::invalid_data, "unknown hibernation mode");
      }
      return std::make_unique<placement::HibernationAction>(*probes_.system, *probes_.tools, *mode);
    }
    if (plan.action == "SetPageFile") {
      Result<placement::PageFileConfig> config = page_file_config(params.at("config"));
      if (!config) {
        return tl::unexpected(config.error());
      }
      return std::make_unique<placement::PageFileAction>(*probes_.system, *probes_.drives,
                                                         *std::move(config));
    }
    if (plan.action == "SetShadowStorage") {
      const Json& maximum = params.at("maximum");
      return std::make_unique<placement::ShadowStorageAction>(
          *probes_.system, *probes_.tools, params.at("volume").get<std::string>(),
          maximum.is_null() ? std::nullopt
                            : std::optional<Bytes>(Bytes{maximum.get<std::uint64_t>()}));
    }
  } catch (const Json::exception& error) {
    return fail(Errc::invalid_data, plan.action + " parameters: " + error.what());
  }
  return fail(Errc::invalid_data, "the service runs no action " + plan.action);
}

Result<actions::RunReport> Executor::execute(PlanId id) {
  Result<Plan> plan = fail(Errc::not_found, "plan not read");
  Result<PlanSpec> spec = fail(Errc::not_found, "plan not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        plan = store::get_plan(db, id);
        if (plan) {
          spec = spec_of(db, *plan);
        }
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!plan || !spec) {
    return tl::unexpected(plan ? spec.error() : plan.error());
  }
  if (spec->scope != ExecutionContext::service) {
    return fail(Errc::invalid_state, "plan " + std::to_string(id.value()) + " runs in the tray");
  }
  const Actor actor =
      plan->proposer.kind() == Actor::Kind::scheduler && spec->tier == Tier::automatic
          ? Actor::scheduler()
          : Actor::owner();
  Result<std::unique_ptr<Action>> action =
      fail(Errc::refused,
           "Windows handlers, DISM and system settings are not applied in console "
           "mode");
  if (options_.system_changes || plan->action == "DeleteItems" ||
      plan->action == "QuarantineItems") {
    action = action_for(*plan, *spec, actor);
  }
  if (!action) {
    // The plan cannot run: it fails with the reason instead of waiting for ever.
    if (const Result<void> started = engine_->transition(id, PlanState::applying, actor); started) {
      static_cast<void>(engine_->transition(id, PlanState::failed, actor, action.error().message));
    }
    return tl::unexpected(action.error());
  }
  Applying applying{.kind = Applying::Kind::plan,
                    .plan = id,
                    .title = spec->title,
                    .started_at = environment_.clock->now()};
  for (const PlanItem& item : plan->items) {
    if (item.approval != ItemApproval::not_approved) {
      ++applying.items_total;
      applying.bytes_total += item.size;
    }
  }
  progress_.start(std::move(applying));
  Result<actions::RunReport> report =
      engine_->execute(id, **action, actor, optimization(spec->kind));
  progress_.finish();
  if (report) {
    raise_alerts(id, *spec, *report);
  }
  if (report && plan->action == "DeleteItems") {
    if (Result<void> manifest = write_manifest(id); !manifest) {
      return tl::unexpected(manifest.error());
    }
  }
  return report;
}

Result<std::pair<Plan, PlanSpec>> Executor::stored_plan(PlanId id) const {
  Result<Plan> plan = fail(Errc::not_found, "plan not read");
  Result<PlanSpec> spec = fail(Errc::not_found, "plan not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        plan = store::get_plan(db, id);
        if (plan) {
          spec = spec_of(db, *plan);
        }
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!plan || !spec) {
    return tl::unexpected(plan ? spec.error() : plan.error());
  }
  return std::pair{*std::move(plan), *std::move(spec)};
}

Result<std::vector<PlanId>> Executor::pending_verifications() const {
  Result<std::vector<PlanId>> pending = fail(Errc::io_error, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        pending = actions::pending_verifications(db);
        if (!pending) {
          return {};
        }
        std::erase_if(*pending, [&](PlanId id) {
          Result<Plan> plan = store::get_plan(db, id);
          Result<PlanSpec> spec =
              plan ? spec_of(db, *plan) : Result<PlanSpec>(tl::unexpected(plan.error()));
          return !spec || spec->scope != ExecutionContext::service;
        });
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  return pending;
}

Result<actions::RunReport> Executor::verify_pending(PlanId id) {
  Result<std::pair<Plan, PlanSpec>> stored = stored_plan(id);
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  const auto& [plan, spec] = *stored;
  const Actor actor = plan.proposer.kind() == Actor::Kind::scheduler && spec.tier == Tier::automatic
                          ? Actor::scheduler()
                          : Actor::owner();
  if (!options_.system_changes && plan.action != "DeleteItems" &&
      plan.action != "QuarantineItems") {
    return fail(Errc::refused, "system settings are not measured in console mode");
  }
  Result<std::unique_ptr<Action>> action = action_for(plan, spec, actor);
  if (!action) {
    return tl::unexpected(action.error());
  }
  Result<actions::RunReport> report =
      engine_->verify_pending(id, **action, actor, optimization(spec.kind));
  if (report) {
    raise_alerts(id, spec, *report);
  }
  return report;
}

void Executor::raise_alerts(PlanId id, const PlanSpec& spec,
                            const actions::RunReport& report) const {
  if (probes_.alerts == nullptr) {
    return;
  }
  if (const std::vector<alerts::Reading> readings = verification_readings(id, spec, report);
      !readings.empty()) {
    probes_.alerts->report(readings);
  }
}

Result<void> Executor::write_manifest(PlanId id) const {
  return environment_.store->write([&](store::Database& db) -> Result<void> {
    Result<std::vector<actions::LogEntry>> entries = actions::plan_log(db, id);
    if (!entries) {
      return tl::unexpected(entries.error());
    }
    std::uint64_t freed = 0;
    std::int64_t removed = 0;
    std::int64_t skipped = 0;
    std::map<std::string, std::uint64_t> by_rule;
    for (const actions::LogEntry& entry : *entries) {
      if (entry.event != "apply" || !entry.item) {
        continue;
      }
      const bool gone = entry.outcome == "removed" || entry.outcome == "partial";
      if (!gone) {
        ++skipped;
        continue;
      }
      ++removed;
      const std::uint64_t bytes = freed_of(entry);
      freed += bytes;
      by_rule[rule_of(db, *entry.item)] += bytes;
    }
    std::string detail;
    for (const auto& [rule, bytes] : by_rule) {
      detail += (detail.empty() ? "" : "; ") + rule + " freed=" + std::to_string(bytes);
    }
    const Result<std::int64_t> logged = actions::append_log(
        db, actions::LogEntry{.time = environment_.clock->now(),
                              .plan = id,
                              .actor = Actor::scheduler().to_string(),
                              .event = "manifest",
                              .outcome = "completed",
                              .detail = detail,
                              .metrics = "freed=" + std::to_string(freed) +
                                         " removed=" + std::to_string(removed) +
                                         " skipped=" + std::to_string(skipped)});
    return logged ? Result<void>{} : tl::unexpected(logged.error());
  });
}

Result<PlanState> Executor::undo(PlanId id) {
  Result<Plan> plan = fail(Errc::not_found, "plan not read");
  Result<PlanSpec> spec = fail(Errc::not_found, "plan not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        plan = store::get_plan(db, id);
        if (plan) {
          spec = spec_of(db, *plan);
        }
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!plan || !spec) {
    return tl::unexpected(plan ? spec.error() : plan.error());
  }
  if (spec->scope != ExecutionContext::service) {
    return fail(Errc::invalid_state, "plan " + std::to_string(id.value()) + " runs in the tray");
  }
  Result<std::unique_ptr<Action>> action = action_for(*plan, *spec, Actor::owner());
  if (!action) {
    return tl::unexpected(action.error());
  }
  if (Result<void> undone = engine_->undo(id, **action, Actor::owner()); !undone) {
    return tl::unexpected(undone.error());
  }
  return PlanState::rolled_back;
}

Result<std::vector<PlanItem>> Executor::items_of(std::span<const std::int64_t> ids,
                                                 std::vector<ItemNote>& notes) const {
  std::vector<std::int64_t> unique;
  for (const std::int64_t id : ids) {
    if (std::ranges::find(unique, id) == unique.end()) {
      unique.push_back(id);
    }
  }
  Result<std::vector<Candidate>> candidates = fail(Errc::not_found, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        candidates = candidates_by_id(db, unique);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  std::vector<PlanItem> items;
  for (const Candidate& candidate : *candidates) {
    if (candidate.strategy == "vhdx") {
      return fail(Errc::invalid_argument,
                  "candidate " + std::to_string(candidate.id) +
                      " is a virtual disk: plans.propose_compact compacts it");
    }
    if (candidate.path.empty() || candidate.strategy == "native" ||
        candidate.strategy == "handler") {
      return fail(Errc::invalid_argument,
                  "candidate " + std::to_string(candidate.id) + " is not a file or folder");
    }
    const Result<os::FileStat> status = environment_.file_system->stat(candidate.path);
    if (!status) {
      return fail(Errc::not_found, "candidate " + std::to_string(candidate.id) + " (" +
                                       utf8(candidate.path) +
                                       ") is gone since the list was made; refresh the list");
    }
    items.push_back(
        PlanItem{.path = candidate.path, .identity = status->identity, .size = candidate.size});
    notes.push_back(ItemNote{.rule = candidate.rule, .reason = candidate.reason});
  }
  return items;
}

Result<Plan> Executor::propose_cleanup(std::span<const std::int64_t> ids, const Actor& proposer) {
  std::vector<ItemNote> notes;
  Result<std::vector<PlanItem>> items = items_of(ids, notes);
  if (!items) {
    return tl::unexpected(items.error());
  }
  const std::size_t count = items->size();
  const PlanSpec spec{
      .kind = PlanKind::cleanup,
      .tier = Tier::ask,
      .scope = ExecutionContext::service,
      .title = "Quarantine " + std::to_string(count) + " items proposed by " + proposer.to_string(),
      .commands = {"move " + std::to_string(count) + " items to the quarantine for 7 days"},
      .undo = "restore from the quarantine until it is purged after 7 days"};
  return submit(*engine_, *environment_.store,
                Plan{.action = "QuarantineItems",
                     .proposer = proposer,
                     .created_at = environment_.clock->now(),
                     .items = *std::move(items)},
                spec, notes, actions::AutomationLevel::suggest);
}

Result<Plan> Executor::recycle(std::span<const std::int64_t> ids) {
  std::vector<ItemNote> notes;
  Result<std::vector<PlanItem>> items = items_of(ids, notes);
  if (!items) {
    return tl::unexpected(items.error());
  }
  const std::size_t count = items->size();
  const PlanSpec spec{.kind = PlanKind::cleanup,
                      .tier = Tier::report,
                      .scope = ExecutionContext::user,
                      .title = "Send " + std::to_string(count) + " items to the Recycle Bin",
                      .commands = {"IFileOperation delete with FOFX_RECYCLEONDELETE, " +
                                   std::to_string(count) + " items"},
                      .undo = "restore them from the Recycle Bin",
                      .params = Json{{"recycle", true}}};
  Result<Plan> stored = submit(*engine_, *environment_.store,
                               Plan{.action = "RecycleItems",
                                    .proposer = Actor::owner(),
                                    .created_at = environment_.clock->now(),
                                    .items = *std::move(items)},
                               spec, notes, actions::AutomationLevel::suggest);
  return approved_by_owner(*engine_, std::move(stored));
}

Result<std::pair<Plan, PlanSpec>> Executor::archive_plan(const Candidate& candidate) const {
  const fs::path& source = candidate.path;
  const Result<os::FileStat> status = environment_.file_system->stat(source);
  if (!status) {
    return fail(Errc::not_found, utf8(source) + " is gone since the list was made; refresh it");
  }
  Result<fs::path> normalized = safety::normalize_path(source);
  if (!normalized) {
    return tl::unexpected(normalized.error());
  }
  if (scanall::system_location(*normalized)) {
    return fail(Errc::refused, utf8(source) + " holds the system or installed programs");
  }
  if (const std::optional<std::string_view> rule = safety::denied_by(*normalized, locations_)) {
    return fail(Errc::refused,
                "the deny-list protects " + utf8(source) + " (" + std::string(*rule) + ")");
  }
  if (probes_.drives == nullptr) {
    return fail(Errc::invalid_state, "the drives cannot be read here");
  }
  Result<std::vector<placement::TargetVolume>> volumes = probes_.drives->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  const std::optional<fs::path> target =
      scanall::archive_target(scanall::drive_infos(*volumes), *normalized, candidate.size);
  if (!target) {
    return fail(Errc::invalid_state, "no healthy HDD keeps 10 % free after taking " +
                                         scanall::size_text(candidate.size));
  }
  const std::string from = utf8(*normalized);
  const std::string to = utf8(*target);
  return std::pair{
      Plan{.action = "ArchiveFolder",
           .proposer = Actor::owner(),
           .created_at = environment_.clock->now(),
           .items = {PlanItem{
               .path = *normalized, .identity = status->identity, .size = candidate.size}}},
      PlanSpec{.kind = PlanKind::archive,
               .tier = Tier::ask,
               .scope = ExecutionContext::user,
               .title = "Move " + from + " to the HDD archive",
               .commands = {"copy " + from + " to " + to +
                            ", check that the copy holds the same files and bytes, then remove " +
                            from + " through the safety checks"},
               .undo = "copy it back from " + to + ", then remove the archive copy",
               .params = Json{{"source", from}, {"target", to}}}};
}

Result<std::pair<Plan, PlanSpec>> Executor::uninstall_plan(const Candidate& candidate) const {
  const std::string& key = candidate.command;
  const std::string& name = candidate.handler;
  std::string command = "the UninstallString of " + key;
  if (probes_.owner_tools != nullptr && probes_.owner_tools->registry != nullptr) {
    Result<std::optional<scanall::UninstallEntry>> entry =
        scanall::uninstall_entry(*probes_.owner_tools->registry, key);
    if (!entry) {
      return tl::unexpected(entry.error());
    }
    const std::optional<scanall::UninstallEntry>& program = *entry;
    if (!program) {
      return fail(Errc::not_found, name + " is not installed any more; refresh the list");
    }
    command = program->uninstall;
  }
  return std::pair{
      Plan{.action = "UninstallProgram",
           .proposer = Actor::owner(),
           .created_at = environment_.clock->now(),
           .items = {PlanItem{.path = from_utf8("uninstall:" + name), .size = candidate.size}}},
      PlanSpec{.kind = PlanKind::uninstall,
               .tier = Tier::ask,
               .scope = ExecutionContext::user,
               .title = "Uninstall " + name,
               .commands = {command},
               .undo = "not possible: install it again with its installer",
               .warnings = {"the program's own uninstaller opens on your desktop; finish it there"},
               .params = Json{{"key", key}, {"name", name}}}};
}

Result<Plan> Executor::owner_action(std::int64_t id) {
  const std::array<std::int64_t, 1> ids = {id};
  Result<std::vector<Candidate>> candidates = fail(Errc::not_found, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        candidates = candidates_by_id(db, ids);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  const Candidate& candidate = candidates->front();
  Result<std::pair<Plan, PlanSpec>> built =
      fail(Errc::invalid_argument, "candidate " + std::to_string(id) + " is no owner decision");
  if (candidate.source == owner_decisions_source && candidate.strategy == "archive") {
    built = archive_plan(candidate);
  } else if (candidate.source == owner_decisions_source && candidate.strategy == "uninstall") {
    built = uninstall_plan(candidate);
  }
  if (!built) {
    return tl::unexpected(built.error());
  }
  const std::array<ItemNote, 1> notes = {ItemNote{.rule = {}, .reason = candidate.reason}};
  return approved_by_owner(
      *engine_, submit(*engine_, *environment_.store, std::move(built->first), built->second, notes,
                       actions::AutomationLevel::suggest));
}

Result<Plan> Executor::propose_uninstall(const apps::App& app, const Actor& proposer) {
  std::vector<std::string> warnings;
  if (app.verdict == apps::Verdict::keep) {
    warnings.push_back("MyClean's verdict is keep: " + app.reason);
  } else if (app.verdict == apps::Verdict::unknown) {
    warnings.push_back("MyClean does not know whether it is safe to remove: " + app.reason);
  }
  warnings.emplace_back(
      "it cannot be undone: only installing it again brings it back; MyClean deletes no folder "
      "or registry entry the uninstaller leaves behind");
  Plan plan{.action = "UninstallProgram",
            .proposer = proposer,
            .created_at = environment_.clock->now(),
            .items = {PlanItem{.path = from_utf8("uninstall:" + app.name),
                               .size = app.size.value_or(Bytes{})}}};
  PlanSpec spec{.kind = PlanKind::uninstall,
                .tier = Tier::ask,
                .scope = ExecutionContext::user,
                .title = "Uninstall " + app.name,
                .undo = "not possible: install it again with its installer"};
  if (app.kind == apps::Kind::appx) {
    spec.commands = {"remove the app package " + app.key + " (PackageManager.RemovePackageAsync)"};
    spec.params = Json{{"package", app.key}, {"name", app.name}};
  } else {
    if (app.uninstall.empty()) {
      return fail(Errc::invalid_argument, app.name + ": no uninstaller registered");
    }
    if (probes_.owner_tools == nullptr || probes_.owner_tools->registry == nullptr) {
      return fail(Errc::invalid_state, "the owner's registry cannot be read here");
    }
    // The command comes from the registry now, never from the request.
    Result<std::optional<scanall::UninstallEntry>> entry =
        scanall::uninstall_entry(*probes_.owner_tools->registry, app.key);
    if (!entry) {
      return tl::unexpected(entry.error());
    }
    if (!*entry || (*entry)->name != app.name) {
      return fail(Errc::not_found, app.name + " is not installed any more; list the apps again");
    }
    spec.commands = {(*entry)->uninstall};
    spec.params = Json{{"key", app.key}, {"name", app.name}};
    warnings.emplace_back("the program's own uninstaller opens on your desktop; finish it there");
  }
  spec.warnings = std::move(warnings);
  return submit(*engine_, *environment_.store, std::move(plan), spec, {},
                actions::AutomationLevel::suggest);
}

Result<Plan> Executor::submit_setting(Action& action, const std::string& name, const PlanSpec& spec,
                                      const Actor& proposer) {
  Result<Plan> plan = placement::plan_of(action, name, proposer, environment_.clock->now());
  if (!plan) {
    return plan;
  }
  return submit(*engine_, *environment_.store, *std::move(plan), spec, {},
                actions::AutomationLevel::suggest);
}

Result<Plan> Executor::propose_optimization(const Optimization& request, const Actor& proposer) {
  if (probes_.system == nullptr || probes_.tools == nullptr || probes_.drives == nullptr) {
    return fail(Errc::invalid_state, "the system settings cannot be read here");
  }
  Result<std::vector<placement::TargetVolume>> volumes = probes_.drives->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  if (request.kind == "hibernation") {
    return propose_hibernation(request, proposer);
  }
  if (request.kind == "page_file") {
    return propose_page_file(request, *volumes, proposer);
  }
  if (request.kind == "shadow_storage") {
    return propose_shadow_storage(request, *volumes, proposer);
  }
  if (request.kind == "relocate") {
    return request.recipe ? propose_recipe(request, *volumes, proposer)
                          : propose_relocation(request, *volumes, proposer);
  }
  return fail(Errc::invalid_argument, "unknown optimization \"" + request.kind + "\"");
}

Result<Plan> Executor::propose_startup(std::span<const std::string> ids, bool enable,
                                       const Actor& proposer) {
  if (probes_.startup == nullptr) {
    return fail(Errc::invalid_state, "the startup items cannot be read here");
  }
  if (ids.empty()) {
    return fail(Errc::invalid_argument, "no startup item named");
  }
  const std::vector<std::string> items(ids.begin(), ids.end());
  startup::StartupToggle action(*probes_.startup, *environment_.clock, items, enable);
  const std::string verb = enable ? "Enable" : "Disable";
  return submit_setting(
      action, std::string(startup::toggle_action_name),
      PlanSpec{.kind = PlanKind::system_setting,
               .tier = Tier::ask,
               .scope = ExecutionContext::service,
               .title = verb + " " + std::to_string(items.size()) + " startup item" +
                        (items.size() == 1 ? "" : "s"),
               .commands = {verb + " through StartupApproved, the task's Enabled flag or the "
                                   "service's start type (Manual)"},
               .undo = "restores each item's previous state exactly",
               .params = Json{{"items", items}, {"enable", enable}}},
      proposer);
}

Result<Plan> Executor::propose_index_exclusion(std::span<const std::int64_t> ids,
                                               const Actor& proposer) {
  if (probes_.indexer == nullptr) {
    return fail(Errc::invalid_state, "the Windows Search scope cannot be read here");
  }
  if (ids.empty()) {
    return fail(Errc::invalid_argument, "no folder named");
  }
  Result<std::vector<Candidate>> candidates = fail(Errc::not_found, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        candidates = candidates_by_id(db, ids);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  std::vector<fs::path> folders;
  std::vector<std::string> stored;
  for (const Candidate& candidate : *candidates) {
    if (candidate.source != indexed_folders_source) {
      return fail(Errc::invalid_argument,
                  "candidate " + std::to_string(candidate.id) + " is no indexed folder");
    }
    folders.push_back(candidate.path);
    stored.push_back(utf8_text(candidate.path));
  }
  indexer::IndexerExclusion action(*probes_.indexer, folders);
  return submit_setting(
      action, std::string(indexer::exclusion_action_name),
      PlanSpec{.kind = PlanKind::system_setting,
               .tier = Tier::ask,
               .scope = ExecutionContext::service,
               .title = "Exclude " + std::to_string(folders.size()) + " folder" +
                        (folders.size() == 1 ? "" : "s") + " from Windows Search",
               .commands = {"Add a Windows Search exclusion rule for each folder "
                            "(ISearchCrawlScopeManager::AddUserScopeRule); the files stay"},
               .undo = "removes each rule added, or puts back the rule it replaced: the "
                       "index scope is as before",
               .params = Json{{"folders", stored}}},
      proposer);
}

Result<Plan> Executor::propose_pause(std::int64_t suspect, std::chrono::hours period,
                                     const Actor& proposer) {
  if (probes_.pauses == nullptr) {
    return fail(Errc::invalid_state, "freeze suspects cannot be paused here");
  }
  if (period < std::chrono::hours{1} || period > elimination::max_pause) {
    return fail(Errc::invalid_argument,
                "a pause lasts 1 to " + std::to_string(elimination::max_pause.count()) + " hours");
  }
  Result<std::vector<input::FreezeGroup>> groups = input::freeze_summary(*environment_.store);
  if (!groups) {
    return tl::unexpected(groups.error());
  }
  std::vector<input::StoredIncident> incidents;
  for (input::FreezeGroup& group : *groups) {
    std::ranges::move(group.incidents, std::back_inserter(incidents));
  }
  const std::optional<elimination::Suspect> chosen = elimination::suspect_by_id(incidents, suspect);
  if (!chosen) {
    return fail(Errc::not_found, "no freeze suspect " + std::to_string(suspect));
  }
  Result<std::vector<WallTime>> times = input::incident_times(*environment_.store);
  if (!times) {
    return tl::unexpected(times.error());
  }
  Result<std::vector<elimination::Span>> periods = fail(Errc::io_error, "not read");
  Result<bool> open = fail(Errc::io_error, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        periods = elimination::pause_periods(db);
        Result<store::Statement> query = db.prepare(
            "SELECT COUNT(*) FROM plan p JOIN plan_params pp ON pp.plan_id = p.id WHERE "
            "p.action = ?1 AND p.state IN ('proposed', 'auto', 'awaiting_approval', 'approved', "
            "'applying', 'applied') AND json_extract(pp.params, '$.params.image') = ?2");
        if (!query) {
          open = tl::unexpected(query.error());
          return {};
        }
        query->bind(1, elimination::pause_action_name).bind(2, std::string_view{chosen->image});
        const Result<bool> row = query->step();
        open = row ? Result<bool>(query->column_int(0) > 0) : tl::unexpected(row.error());
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!periods) {
    return tl::unexpected(periods.error());
  }
  if (!open) {
    return tl::unexpected(open.error());
  }
  if (*open) {
    return fail(Errc::already_exists, "a pause of " + chosen->image + " is waiting or active");
  }
  if (!elimination::baseline(*times, environment_.clock->now(), *periods)) {
    return fail(Errc::invalid_state, "a baseline of freeze incidents is still being collected");
  }
  const elimination::PauseMode mode = chosen->mode();
  const std::string hours = std::to_string(period.count()) + " hours";
  PlanSpec spec{.kind = PlanKind::pause,
                .tier = Tier::ask,
                .scope = ExecutionContext::service,
                .params = Json{{"image", chosen->image},
                               {"mode", std::string(elimination::to_string(mode))},
                               {"hours", period.count()}}};
  if (mode == elimination::PauseMode::guided) {
    spec.title = "Quit " + chosen->image + " for " + hours + " to test the freezes";
    spec.commands = {"Nothing is suspended, closed or started: quit " + chosen->image +
                     " yourself; MyClean counts only the hours it is not running"};
    spec.undo = "ends the pause; start " + chosen->image + " again yourself";
    spec.warnings = {chosen->image +
                     " owns a keyboard hook: suspended, it would make every "
                     "keystroke wait and stop working until restarted, so MyClean does not "
                     "suspend it. Quit it for " +
                     hours +
                     "; MyClean reminds you to start it "
                     "again when the pause is over"};
  } else {
    spec.title = "Pause " + chosen->image + " for " + hours + " to test the freezes";
    spec.commands = {"Suspend every " + chosen->image +
                     " process of your session (except the "
                     "foreground app), and resume each after " +
                     hours};
    spec.undo = "resumes every process the pause suspended";
    spec.warnings = {chosen->image +
                     " stops responding until the pause is over or you end it "
                     "in the Freezes view"};
  }
  elimination::PauseSuspect action(*probes_.pauses, PlanId{}, chosen->image, mode, period);
  return submit_setting(action, std::string(elimination::pause_action_name), spec, proposer);
}

Result<Plan> Executor::propose_compaction(std::span<const std::int64_t> ids,
                                          const Actor& proposer) {
  if (probes_.disks == nullptr || probes_.wsl == nullptr) {
    return fail(Errc::invalid_state, "virtual disks cannot be compacted here");
  }
  if (ids.empty()) {
    return fail(Errc::invalid_argument, "no virtual disk named");
  }
  Result<std::vector<Candidate>> candidates = fail(Errc::not_found, "not read");
  if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
        candidates = candidates_by_id(db, ids);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!candidates) {
    return tl::unexpected(candidates.error());
  }
  std::vector<fs::path> disks;
  Json paths = Json::array();
  std::vector<std::string> commands = {"wsl.exe --shutdown (as the owner)"};
  for (const Candidate& candidate : *candidates) {
    if (candidate.source != vhdx_source || candidate.strategy != "vhdx") {
      return fail(Errc::invalid_argument,
                  "candidate " + std::to_string(candidate.id) + " is not a disk of space.vhdx");
    }
    disks.push_back(candidate.path);
    paths.push_back(utf8(candidate.path));
    commands.push_back("CompactVirtualDisk " + utf8(candidate.path));
  }
  vhdx::CompactVhdx action(*environment_.file_system, *probes_.wsl, *probes_.disks, disks);
  return submit_setting(
      action, std::string(vhdx::compact_action_name),
      PlanSpec{.kind = PlanKind::compact,
               .tier = Tier::ask,
               .scope = ExecutionContext::service,
               .title = "Compact " + std::to_string(disks.size()) + " WSL/Docker virtual disk" +
                        (disks.size() == 1 ? "" : "s"),
               .commands = std::move(commands),
               .undo = "not reversible: compaction only gives unused space back; the disks keep "
                       "their data",
               .warnings = {"Every running WSL distribution and Docker Desktop's containers stop "
                            "first (wsl --shutdown): save your work in them and quit Docker "
                            "Desktop before approving.",
                            "A disk still in use when its turn comes (Docker Desktop started it "
                            "again) is skipped. Compacting a large disk can take a long time."},
               .params = Json{{"disks", std::move(paths)}}},
      proposer);
}

Result<Plan> Executor::propose_hibernation(const Optimization& request, const Actor& proposer) {
  Result<placement::HibernationState> state = probes_.system->hibernation();
  if (!state) {
    return tl::unexpected(state.error());
  }
  const std::optional<placement::HibernationMode> mode =
      request.mode.empty() ? placement::recommended_hibernation(*state)
                           : placement::parse_hibernation_mode(request.mode);
  if (!mode || *mode == state->mode) {
    return fail(Errc::invalid_state, "hibernation needs no change");
  }
  placement::HibernationAction action(*probes_.system, *probes_.tools, *mode);
  Result<placement::SettingSummary> summary = action.summary();
  if (!summary) {
    return tl::unexpected(summary.error());
  }
  return submit_setting(
      action, "SetHibernation",
      setting_spec(*summary, "Hibernation", Json{{"mode", placement::to_string(*mode)}}), proposer);
}

Result<Plan> Executor::propose_page_file(const Optimization& request,
                                         std::vector<placement::TargetVolume> volumes,
                                         const Actor& proposer) {
  Result<placement::PageFileState> state = probes_.system->page_files();
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (!request.target.empty()) {
    Result<fs::path> root = drive_root_of(request.target);
    if (!root) {
      return tl::unexpected(root.error());
    }
    std::erase_if(volumes, [&](const placement::TargetVolume& volume) {
      return !placement::same_path(volume.root, *root) &&
             !placement::same_path(volume.root, options_.system_root);
    });
  }
  const std::optional<placement::PageFileConfig> config =
      placement::recommended_page_files(*state, volumes, options_.system_root);
  if (!config) {
    return fail(Errc::invalid_state,
                "the page file needs no change, or no healthy volume has room for it");
  }
  placement::PageFileAction action(*probes_.system, *probes_.drives, *config);
  Result<placement::SettingSummary> summary = action.summary();
  if (!summary) {
    return tl::unexpected(summary.error());
  }
  return submit_setting(
      action, "SetPageFile",
      setting_spec(*summary, "Page file", Json{{"config", page_file_json(*config)}}), proposer);
}

Result<Plan> Executor::submit_automatic(Action& action, const std::string& name, PlanSpec spec,
                                        const std::string& group) {
  Result<Plan> plan =
      placement::plan_of(action, name, Actor::scheduler(), environment_.clock->now());
  if (!plan) {
    return plan;
  }
  spec.tier = Tier::automatic;
  spec.group = group;
  return submit(*engine_, *environment_.store, *std::move(plan), spec, {},
                actions::AutomationLevel::automatic);
}

Result<std::optional<PlanId>> Executor::automatic_hibernation() {
  Result<placement::HibernationState> state = probes_.system->hibernation();
  if (!state) {
    return tl::unexpected(state.error());
  }
  const std::optional<placement::HibernationMode> mode = placement::recommended_hibernation(*state);
  if (!mode) {
    return std::nullopt;
  }
  placement::HibernationAction action(*probes_.system, *probes_.tools, *mode);
  Result<placement::SettingSummary> summary = action.summary();
  if (!summary) {
    return tl::unexpected(summary.error());
  }
  Result<Plan> stored = submit_automatic(
      action, "SetHibernation",
      setting_spec(*summary, "Hibernation", Json{{"mode", placement::to_string(*mode)}}),
      std::string(hibernation_group));
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  return stored->id;
}

Result<std::optional<PlanId>> Executor::automatic_page_file(
    std::span<const placement::TargetVolume> volumes) {
  Result<placement::PageFileState> state = probes_.system->page_files();
  if (!state) {
    return tl::unexpected(state.error());
  }
  const std::optional<placement::PageFileConfig> config =
      placement::recommended_page_files(*state, volumes, options_.system_root);
  if (!config) {
    return std::nullopt;
  }
  placement::PageFileAction action(*probes_.system, *probes_.drives, *config);
  Result<placement::SettingSummary> summary = action.summary();
  if (!summary) {
    return tl::unexpected(summary.error());
  }
  Result<Plan> stored = submit_automatic(
      action, "SetPageFile",
      setting_spec(*summary, "Page file", Json{{"config", page_file_json(*config)}}),
      std::string(page_file_group));
  if (!stored) {
    return tl::unexpected(stored.error());
  }
  return stored->id;
}

Result<void> Executor::automatic_relocations(std::span<const packs::RelocationRecipe> recipes,
                                             std::span<const placement::TargetVolume> volumes,
                                             const std::set<std::string>& open,
                                             std::vector<PlanId>& plans) {
  if (probes_.owner_tools == nullptr || probes_.commands == nullptr) {
    return {};
  }
  for (const packs::RelocationRecipe& recipe : recipes) {
    // A needed program that runs keeps the recipe for the next scan.
    if (!recipe.automatic || !placement::recipe_idle(*probes_.owner_tools, recipe)) {
      continue;
    }
    Result<std::vector<placement::RecipeLocation>> locations =
        placement::recipe_locations(*probes_.owner_tools, *probes_.commands, recipe);
    if (!locations) {
      return tl::unexpected(locations.error());
    }
    for (const placement::RecipeLocation& location : *locations) {
      const placement::TargetVolume* volume = placement::volume_of(volumes, location.path);
      const std::string group =
          "auto:relocate:" + recipe.id + (location.instance.empty() ? "" : ":" + location.instance);
      if (volume == nullptr || !placement::same_path(volume->root, options_.system_root) ||
          open.contains(group)) {
        continue;
      }
      Result<Plan> stored = propose_recipe(
          Optimization{.kind = "relocate", .recipe = recipe, .instance = location.instance},
          {volumes.begin(), volumes.end()}, Actor::scheduler(), group);
      if (!stored) {
        return tl::unexpected(stored.error());
      }
      plans.push_back(stored->id);
    }
  }
  return {};
}

Result<std::vector<PlanId>> Executor::reclaim_system_space(
    std::span<const packs::RelocationRecipe> recipes) {
  std::vector<PlanId> plans;
  if (!options_.system_changes || probes_.system == nullptr || probes_.tools == nullptr ||
      probes_.drives == nullptr) {
    return plans;
  }
  Result<std::vector<placement::TargetVolume>> volumes = probes_.drives->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  if (!low_on_space(placement::volume_of(*volumes, options_.system_root))) {
    return plans;
  }
  // A change that failed or was undone is not tried again for a week.
  const Result<std::set<std::string>> open =
      open_groups(environment_.clock->now() - std::chrono::days{7});
  if (!open) {
    return tl::unexpected(open.error());
  }
  for (const std::string_view group : {hibernation_group, page_file_group}) {
    if (open->contains(std::string(group))) {
      continue;
    }
    const Result<std::optional<PlanId>> plan =
        group == hibernation_group ? automatic_hibernation() : automatic_page_file(*volumes);
    if (!plan) {
      return tl::unexpected(plan.error());
    }
    if (*plan) {
      plans.push_back(**plan);
    }
  }
  if (Result<void> moved = automatic_relocations(recipes, *volumes, *open, plans); !moved) {
    return tl::unexpected(moved.error());
  }
  return plans;
}

Result<Plan> Executor::propose_shadow_storage(const Optimization& request,
                                              std::vector<placement::TargetVolume> volumes,
                                              const Actor& proposer) {
  Result<fs::path> root = request.volume.empty() ? Result<fs::path>(options_.system_root)
                                                 : drive_root_of(request.volume);
  if (!root) {
    return tl::unexpected(root.error());
  }
  Result<std::vector<placement::ShadowStorage>> storages = probes_.system->shadow_storage();
  if (!storages) {
    return tl::unexpected(storages.error());
  }
  const std::string name = drive_name(*root);
  const auto storage = std::ranges::find(*storages, name, &placement::ShadowStorage::volume);
  const placement::TargetVolume* volume = placement::volume_of(volumes, *root);
  if (storage == storages->end() || volume == nullptr) {
    return fail(Errc::not_found, name + " has no shadow storage");
  }
  constexpr double gigabyte = 1024.0 * 1024.0 * 1024.0;
  const std::optional<Bytes> maximum =
      request.maximum_gb
          ? std::optional<Bytes>(
                Bytes{static_cast<std::uint64_t>(std::llround(*request.maximum_gb * gigabyte))})
          : placement::recommended_shadow_maximum(*storage, volume->size);
  if (!maximum || maximum == storage->maximum) {
    return fail(Errc::invalid_state, "the shadow storage of " + name + " needs no change");
  }
  placement::ShadowStorageAction action(*probes_.system, *probes_.tools, name, maximum);
  Result<placement::SettingSummary> summary = action.summary();
  if (!summary) {
    return tl::unexpected(summary.error());
  }
  return submit_setting(action, "SetShadowStorage",
                        setting_spec(*summary, "Shadow storage of " + name,
                                     Json{{"volume", name}, {"maximum", maximum->count()}}),
                        proposer);
}

Result<Plan> Executor::propose_relocation(const Optimization& request,
                                          std::vector<placement::TargetVolume> volumes,
                                          const Actor& proposer) {
  const std::optional<placement::CacheKind> cache = placement::parse_cache_kind(request.cache);
  if (!cache) {
    return fail(Errc::invalid_argument, "unknown cache \"" + request.cache + "\"");
  }
  Result<placement::TargetVolume> target = relocation_volume(request, volumes);
  if (!target) {
    return tl::unexpected(target.error());
  }
  const std::optional<placement::ContentMode> content =
      request.content.empty() ? placement::recommended_content(*cache)
                              : placement::parse_content_mode(request.content);
  if (!content) {
    return fail(Errc::invalid_argument, "content must be move or regenerate");
  }
  const fs::path folder = placement::target_folder(*cache, target->root);
  const std::string cache_name(placement::to_string(*cache));
  const PlanSpec spec{
      .kind = PlanKind::relocate,
      .tier = Tier::ask,
      .scope = ExecutionContext::user,
      .title = "Relocate " + cache_name + " to " + utf8(folder),
      .commands = {"point the " + cache_name + " setting to " + utf8(folder) + " (" +
                   std::string(placement::to_string(*content)) + " the existing content)"},
      .undo = "restores every changed setting to its exact previous value",
      .warnings = {"the current location and size are measured by the tray when it applies"},
      .params = Json{{"cache", cache_name},
                     {"target", utf8(folder)},
                     {"content", placement::to_string(*content)}}};
  return submit(*engine_, *environment_.store,
                Plan{.action = "RelocateCache",
                     .proposer = proposer,
                     .created_at = environment_.clock->now(),
                     .items = {PlanItem{.path = from_utf8("relocate:" + cache_name)}}},
                spec, {}, actions::AutomationLevel::suggest);
}

Result<placement::TargetVolume> Executor::relocation_volume(
    const Optimization& request, std::span<const placement::TargetVolume> volumes) const {
  const placement::TargetVolume* target = nullptr;
  if (request.target.empty()) {
    target = placement::best_target(volumes, options_.system_root, Bytes{});
  } else if (Result<fs::path> root = drive_root_of(request.target); root) {
    target = placement::volume_of(volumes, *root);
  } else {
    return tl::unexpected(root.error());
  }
  if (target == nullptr || !placement::unsuitable(*target, Bytes{}).empty()) {
    return fail(Errc::invalid_state, "no healthy target volume with room");
  }
  return *target;
}

Result<Plan> Executor::propose_recipe(const Optimization& request,
                                      std::vector<placement::TargetVolume> volumes,
                                      const Actor& proposer, const std::string& group) {
  const packs::RelocationRecipe& recipe = *request.recipe;
  const bool instances = !recipe.current.name.empty();
  if (instances == request.instance.empty()) {
    return fail(Errc::invalid_argument,
                instances ? recipe.id + " moves one instance: name it with instance"
                          : recipe.id + " names no instances, so instance must be left out");
  }
  const auto folded = [](std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return text;
  };
  if (std::ranges::any_of(recipe.skip_names, [&](const std::string& skipped) {
        return folded(skipped) == folded(request.instance);
      })) {
    return fail(Errc::refused, request.instance + " is never moved by " + recipe.id);
  }
  Result<placement::TargetVolume> target = relocation_volume(request, volumes);
  if (!target) {
    return tl::unexpected(target.error());
  }
  const std::optional<placement::ContentMode> content = placement::parse_content_mode(
      request.content.empty() ? std::string(packs::to_string(recipe.content)) : request.content);
  if (!content) {
    return fail(Errc::invalid_argument, "content must be move or regenerate");
  }
  // "<pack>/<case>" as the folders <pack>\<case>.
  std::string relative = recipe.id;
  std::ranges::replace(relative, '/', '\\');
  if (instances) {
    relative += "\\" + request.instance;
  }
  const placement::RecipeRelocation relocation{
      .recipe = recipe,
      .instance = request.instance,
      .target = target->root / L"Relocated" / from_utf8(relative),
      .content = *content};
  std::string what = recipe.label;
  std::string item = "relocate:" + recipe.id;
  if (instances) {
    what += " " + request.instance;
    item += ":" + request.instance;
  }
  std::vector<std::string> warnings = {
      "the current location and size are measured by the tray when it applies"};
  std::string undo = recipe.undo == packs::UndoKind::restore
                         ? "restores " + recipe.current.text + " to its exact previous value"
                         : "moves it back the same way to its previous folder";
  if (recipe.undo == packs::UndoKind::none) {
    undo = "not reversible: " + recipe.note;
  } else if (!recipe.note.empty()) {
    warnings.push_back(recipe.note);
  }
  const bool automatic = !group.empty();
  const PlanSpec spec{.kind = PlanKind::relocate,
                      .tier = automatic ? Tier::automatic : Tier::ask,
                      .scope = ExecutionContext::user,
                      .title = "Relocate " + what + " to " + utf8(relocation.target),
                      .group = group,
                      .commands = placement::recipe_commands(relocation),
                      .undo = std::move(undo),
                      .warnings = std::move(warnings),
                      .params = Json{{"recipe", placement::recipe_json(recipe)},
                                     {"instance", request.instance},
                                     {"target", utf8(relocation.target)},
                                     {"content", placement::to_string(*content)}}};
  return submit(
      *engine_, *environment_.store,
      Plan{.action = "RelocateRecipe",
           .proposer = proposer,
           .created_at = environment_.clock->now(),
           .items = {PlanItem{.path = from_utf8(item)}}},
      spec, {},
      automatic ? actions::AutomationLevel::automatic : actions::AutomationLevel::suggest);
}

Result<std::size_t> Executor::restore(PlanId run, std::optional<PlanItemId> item) {
  const auto held = [&]() -> Result<std::size_t> {
    Result<std::vector<actions::QuarantineRecord>> records = fail(Errc::io_error, "not read");
    if (Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
          records = actions::quarantine_records(db, run);
          return {};
        });
        !read) {
      return tl::unexpected(read.error());
    }
    if (!records) {
      return tl::unexpected(records.error());
    }
    return static_cast<std::size_t>(
        std::ranges::count_if(*records, [&](const actions::QuarantineRecord& record) {
          return record.state == actions::QuarantineState::held && (!item || record.item == *item);
        }));
  };
  const Result<std::size_t> before = held();
  if (!before) {
    return tl::unexpected(before.error());
  }
  if (*before == 0) {
    return fail(Errc::not_found, "run " + std::to_string(run.value()) + " holds no such item");
  }
  std::vector<PlanItemId> items;
  if (item) {
    items.push_back(*item);
  }
  const Result<void> restored = quarantine_->restore(run, items, Actor::owner());
  const Result<std::size_t> after = held();
  if (!restored) {
    return tl::unexpected(restored.error());
  }
  if (!after) {
    return tl::unexpected(after.error());
  }
  return *before - *after;
}

Result<std::size_t> Executor::purge() {
  // A quarantine on a volume short of space frees nothing until it is purged: purge it now.
  actions::PurgeEarly early;
  if (probes_.drives != nullptr) {
    if (Result<std::vector<placement::TargetVolume>> volumes = probes_.drives->volumes()) {
      early = [volumes = *std::move(volumes)](const fs::path& folder) {
        return low_on_space(placement::volume_of(volumes, folder));
      };
    }
  }
  progress_.start(Applying{.kind = Applying::Kind::purge,
                           .title = "Purge the quarantine",
                           .started_at = environment_.clock->now()});
  Result<std::size_t> purged =
      quarantine_->purge(Actor::scheduler(), std::nullopt, early, &progress_);
  progress_.finish();
  return purged;
}

Result<std::size_t> Executor::purge_run(PlanId run) {
  progress_.start(Applying{.kind = Applying::Kind::purge,
                           .plan = run,
                           .title = "Delete quarantine run " + std::to_string(run.value()),
                           .started_at = environment_.clock->now()});
  Result<std::size_t> purged = quarantine_->purge(Actor::owner(), run, {}, &progress_);
  progress_.finish();
  return purged;
}

}  // namespace myclean::cleaning
