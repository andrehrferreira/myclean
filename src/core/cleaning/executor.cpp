#include "core/cleaning/executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
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
#include "core/clean/engine.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleanup/dism_cleanup.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/model/action.hpp"
#include "core/model/actor.hpp"
#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/proposal.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/placement/system_space.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

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

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path from_utf8(const std::string& text) {
  return {std::u8string(text.begin(), text.end())};
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

Result<std::set<std::string>> Executor::open_groups() const {
  std::set<std::string> groups;
  const Result<void> read = environment_.store->read([&](store::Database& db) -> Result<void> {
    Result<store::Statement> query = db.prepare(
        "SELECT pp.params FROM plan p JOIN plan_params pp ON pp.plan_id = p.id "
        "WHERE p.state IN ('proposed', 'auto', 'awaiting_approval', 'approved', 'applying')");
    if (!query) {
      return tl::unexpected(query.error());
    }
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
    if (candidate.strategy == rules::Strategy::handler &&
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
                              .reason = reason_of(candidate, now),
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
    return {};  // free space unknown: every item stays on its own volume
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
  Result<actions::RunReport> report = engine_->execute(id, **action, actor, false);
  progress_.finish();
  if (report && plan->action == "DeleteItems") {
    if (Result<void> manifest = write_manifest(id); !manifest) {
      return tl::unexpected(manifest.error());
    }
  }
  return report;
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
  if (!stored) {
    return stored;
  }
  std::vector<PlanItemId> all;
  for (const PlanItem& item : stored->items) {
    all.push_back(item.id);
  }
  Result<PlanState> approved = engine_->approve(stored->id, all, Actor::owner());
  if (!approved) {
    return tl::unexpected(approved.error());
  }
  stored->state = *approved;
  return stored;
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
                                      const Actor& proposer) {
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
  const PlanSpec spec{.kind = PlanKind::relocate,
                      .tier = Tier::ask,
                      .scope = ExecutionContext::user,
                      .title = "Relocate " + what + " to " + utf8(relocation.target),
                      .commands = placement::recipe_commands(relocation),
                      .undo = std::move(undo),
                      .warnings = std::move(warnings),
                      .params = Json{{"recipe", placement::recipe_json(recipe)},
                                     {"instance", request.instance},
                                     {"target", utf8(relocation.target)},
                                     {"content", placement::to_string(*content)}}};
  return submit(*engine_, *environment_.store,
                Plan{.action = "RelocateRecipe",
                     .proposer = proposer,
                     .created_at = environment_.clock->now(),
                     .items = {PlanItem{.path = from_utf8(item)}}},
                spec, {}, actions::AutomationLevel::suggest);
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
