#include "core/cleaning/service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/clean/workspaces.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/records.hpp"
#include "core/cleaning/views.hpp"
#include "core/indexer/folders.hpp"
#include "core/ipc/frame.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/plans.hpp"
#include "core/map/attribution.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/model/units.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/owner_decisions.hpp"
#include "core/sched/schedule_repository.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/installers.hpp"
#include "core/space/json.hpp"
#include "core/space/photos.hpp"
#include "core/space/scanner.hpp"
#include "core/space/top.hpp"
#include "core/startup/boot_times.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/impact.hpp"
#include "core/startup/startup.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"

namespace myclean::cleaning {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using ipc::Access;
using ipc::Param;
using ipc::ParamType;

constexpr std::size_t max_problems = 8;

std::string utf8(const fs::path& path) {
  return utf8_text(path);
}

PlanId plan_param(const Json& params, const char* name = "id") {
  return PlanId{params.at(name).get<std::int64_t>()};
}

std::vector<std::int64_t> ids_of(const Json& record, const char* name) {
  const auto list = record.find(name);
  return list == record.end() ? std::vector<std::int64_t>{}
                              : list->get<std::vector<std::int64_t>>();
}

Actor proposer_of(const Json& params, const ipc::Caller& caller) {
  if (caller.tray) {
    return Actor::owner();
  }
  // The client name is a token, so it is a valid audit-log name.
  return Actor::mcp(params.value("client", std::string("unknown"))).value_or(Actor::owner());
}

std::string text_param(const Json& params, const char* name) {
  return params.value(name, std::string{});
}

// {"plan", "state"} of a created plan.
Result<Json> plan_answer(const Result<Plan>& plan) {
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  return Json{{"plan", plan->id.value()}, {"state", to_string(plan->state)}};
}

Result<Json> state_answer(const Result<PlanState>& state) {
  if (!state) {
    return tl::unexpected(state.error());
  }
  return Json{{"state", to_string(*state)}};
}

// The candidates of an analysis and the ids written back into its JSON entries.
Result<void> store_candidates(store::Store& store, const std::string& source,
                              std::vector<Candidate>& candidates) {
  return store.write(
      [&](store::Database& db) { return replace_candidates(db, source, candidates); });
}

// Keeps the first entries of json["groups"] that fit 3/4 of one pipe message, so the answer fits
// one; sets "groups_total" and "truncated". Every file stays a candidate, so a proposal can name
// any of them.
void fit_groups(Json& json) {
  Json& groups = json["groups"];
  const std::size_t total = groups.size();
  std::size_t kept = 0;
  for (std::size_t bytes = 0; kept < total; ++kept) {
    bytes += groups[kept].dump().size() + 1;
    if (bytes > ipc::max_payload_size / 4 * 3) {
      break;
    }
  }
  groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(kept), groups.end());
  json["groups_total"] = total;
  json["truncated"] = kept < total;
}

// data\workspaces.toml: nullopt when it does not exist, an error when it does not parse.
Result<std::optional<clean::Workspaces>> read_workspaces(const packs::IPackFiles& files,
                                                         const fs::path& data_dir) {
  const fs::path file = data_dir / "workspaces.toml";
  const Result<std::string> text = files.read(file);
  if (!text) {
    return std::optional<clean::Workspaces>{};
  }
  Result<clean::Workspaces> parsed = clean::parse_workspace_file(*text, file);
  if (!parsed) {
    return tl::unexpected(parsed.error());
  }
  return std::optional<clean::Workspaces>{*std::move(parsed)};
}

Candidate file_candidate(const fs::path& path, Bytes size, std::string reason, WallTime now) {
  return Candidate{.category = "space",
                   .tier = Tier::report,
                   .scope = ExecutionContext::service,
                   .strategy = "file",
                   .path = path,
                   .size = size,
                   .reason = std::move(reason),
                   .found_at = now};
}

// cleaning.status "applying" (core/ipc/plans.hpp).
Json applying_json(const std::optional<Applying>& applying) {
  if (!applying) {
    return nullptr;
  }
  return Json{{"kind", std::string(to_string(applying->kind))},
              {"plan", applying->plan ? Json(applying->plan->value()) : Json(nullptr)},
              {"title", applying->title},
              {"started_at", store::to_unix_seconds(applying->started_at)},
              {"items_done", applying->items_done},
              {"items_total", applying->items_total},
              {"bytes_done", applying->bytes_done.count()},
              {"bytes_total", applying->bytes_total.count()},
              {"files_done", applying->files_done},
              {"current", applying->current.empty() ? Json(nullptr) : Json(applying->current)}};
}

}  // namespace

CleaningService::CleaningService(ServiceSources sources, ServiceSettings settings)
    : sources_(std::move(sources)),
      settings_(std::move(settings)),
      engine_(*sources_.store, *sources_.clock),
      plans_(*sources_.store),
      tasks_(*sources_.store, engine_, *sources_.clock),
      repository_(*sources_.store),
      scheduler_(*sources_.clock, repository_) {}

CleaningService::~CleaningService() = default;

Result<void> CleaningService::start(ipc::Dispatcher& methods) {
  if (settings_.execute) {
    if (Result<void> added = scheduler_.add(std::string(auto_job), settings_.auto_schedule);
        !added) {
      return added;
    }
    if (Result<void> added = scheduler_.add(std::string(purge_job), settings_.purge_schedule);
        !added) {
      return added;
    }
  }
  return register_methods(methods);
}

void CleaningService::note(std::string problem) {
  const std::scoped_lock lock(mutex_);
  if (std::ranges::find(problems_, problem) != problems_.end()) {
    return;
  }
  problems_.push_back(std::move(problem));
  if (problems_.size() > max_problems) {
    problems_.erase(problems_.begin());
  }
}

std::vector<std::string> CleaningService::problems() const {
  const std::scoped_lock lock(mutex_);
  return problems_;
}

std::optional<std::string> CleaningService::unavailable() const {
  const std::scoped_lock lock(mutex_);
  if (executor() == nullptr) {
    return "the owner's session is not available yet" +
           (owner_problem_ ? ": " + *owner_problem_ : std::string{});
  }
  if (!settings_.execute) {
    return std::string("console run without a sandbox: scans and proposals only, nothing runs");
  }
  return std::nullopt;
}

Result<Executor*> CleaningService::require_executor() const {
  if (Executor* ready = executor(); ready != nullptr) {
    return ready;
  }
  return fail(Errc::invalid_state, "cleaning is unavailable: " + unavailable().value_or(""));
}

void CleaningService::resolve_owner() {
  const MonoTime now = sources_.clock->monotonic();
  if (owner_tried_ && now - *owner_tried_ < owner_retry) {
    return;
  }
  owner_tried_ = now;
  Result<OwnerContext> owner = fail(Errc::not_found, "no owner source");
  if (sources_.owner) {
    owner = sources_.owner();
  }
  Result<safety::SafeRemove> remove = fail(Errc::not_found, "no owner");
  if (owner) {
    remove = safety::SafeRemove::create(*sources_.file_system, plans_, owner->locations);
  } else {
    remove = tl::unexpected(owner.error());
  }
  if (!remove) {
    const std::scoped_lock lock(mutex_);
    owner_problem_ = remove.error().message;
    return;
  }
  owner_ = std::make_unique<OwnerContext>(*std::move(owner));
  remove_.emplace(*std::move(remove));
  const actions::Environment environment{.store = sources_.store,
                                         .file_system = sources_.file_system,
                                         .safe_remove = &*remove_,
                                         .clock = sources_.clock};
  quarantine_ = std::make_unique<actions::Quarantine>(environment, engine_);
  Probes probes = sources_.probes;
  probes.environment = owner_->environment.get();
  if (owner_->git) {
    probes.git = owner_->git.get();
  }
  probes.owner_tools = owner_->tools.get();
  executor_ = std::make_unique<Executor>(environment, engine_, *quarantine_, probes,
                                         owner_->locations, settings_.executor);
  {
    const std::scoped_lock lock(mutex_);
    owner_problem_.reset();
  }
  ready_ = executor_.get();
  reload_packs();
}

void CleaningService::reload_packs() {
  const Result<std::optional<fs::path>> owner_dir =
      sources_.owner_packs ? sources_.owner_packs()
                           : Result<std::optional<fs::path>>(std::optional<fs::path>{});
  const packs::LoadReport report =
      packs::load_installed(*sources_.files, sources_.data_dir, owner_dir);
  for (const Error& error : report.errors) {
    note("packs: " + error.message);
  }
  const packs::Probes probes{.machine = sources_.machine,
                             .tree = sources_.probes.tree,
                             .environment = owner_->environment.get()};
  registry_ = sources_.machine == nullptr ? packs::make_registry(report.packs)
                                          : packs::make_registry(report.packs, probes);
  std::vector<ipc::PackStatus> status;
  for (const packs::PackOutcome& outcome : report.outcomes) {
    const auto loaded = std::ranges::find_if(report.packs, [&](const packs::Pack& pack) {
      return pack.manifest.id == outcome.id && pack.origin == outcome.origin;
    });
    const bool found = loaded != report.packs.end() && outcome.status == packs::PackStatus::loaded;
    status.push_back(ipc::PackStatus{
        .id = outcome.id,
        .version = found ? packs::to_string(loaded->manifest.version) : std::string{},
        .origin = std::string(packs::to_string(outcome.origin)),
        .status = std::string(packs::to_string(outcome.status)),
        .applies = found && std::ranges::find(registry_.packs, outcome.id) != registry_.packs.end(),
        .message = outcome.message.empty() ? std::nullopt : std::optional(outcome.message)});
  }
  std::vector<map::Area> uninstall_areas;
  std::ranges::copy_if(
      registry_.attribution, std::back_inserter(uninstall_areas),
      [](const map::Area& area) { return area.reclaim.kind == map::ReclaimKind::uninstall; });
  std::vector<map::Area> index_areas;
  std::ranges::copy_if(registry_.attribution, std::back_inserter(index_areas),
                       [](const map::Area& area) { return indexer::kept_out(area.category); });
  std::vector<rules::CleaningRule> project_rules;
  std::ranges::copy_if(
      registry_.clean, std::back_inserter(project_rules),
      [](const rules::CleaningRule& rule) { return rule.project_marker.has_value(); });
  const std::scoped_lock lock(mutex_);
  recipes_ = registry_.relocate;
  uninstall_areas_ = std::move(uninstall_areas);
  index_areas_ = std::move(index_areas);
  project_rules_ = std::move(project_rules);
  pack_status_ = std::move(status);
}

std::optional<packs::RelocationRecipe> CleaningService::recipe(std::string_view id) const {
  const std::scoped_lock lock(mutex_);
  const auto found = std::ranges::find(recipes_, id, &packs::RelocationRecipe::id);
  return found == recipes_.end() ? std::nullopt : std::optional(*found);
}

std::vector<ipc::PackStatus> CleaningService::pack_status() const {
  const std::scoped_lock lock(mutex_);
  return pack_status_;
}

void CleaningService::run_jobs(const std::stop_token& stop) {
  const sched::PollResult due = scheduler_.poll();
  for (const Error& error : due.errors) {
    note("scheduler: " + error.message);
  }
  for (const sched::DueRun& run : due.runs) {
    if (run.job == auto_job) {
      auto_pending_ = true;
    } else if (run.job == purge_job) {
      Result<void> purged;
      if (Executor* ready = executor(); ready == nullptr) {
        purged = fail(Errc::invalid_state, "the owner's session is not available");
      } else if (Result<std::size_t> count = ready->purge(); !count) {
        purged = tl::unexpected(count.error());
      }
      static_cast<void>(scheduler_.finish(purge_job, purged));
    }
  }
  if (!auto_pending_ || stop.stop_requested()) {
    return;
  }
  if (executor() == nullptr) {
    auto_pending_ = false;
    static_cast<void>(scheduler_.finish(
        auto_job, fail(Errc::invalid_state, "the owner's session is not available")));
  } else if (!sources_.busy || !sources_.busy()) {
    auto_pending_ = false;
    run_scan(true, stop);
  }
}

void CleaningService::work(const std::stop_token& stop) {
  if (executor() == nullptr) {
    resolve_owner();
  }
  if (settings_.execute) {
    run_jobs(stop);
  }
  bool requested = false;
  {
    const std::scoped_lock lock(mutex_);
    requested = scan_requested_;
    scan_requested_ = false;
  }
  if (requested && executor() != nullptr) {
    run_scan(false, stop);
  }
  if (settings_.execute && executor() != nullptr && !stop.stop_requested()) {
    run_purges();
    run_approved();
  }
}

void CleaningService::run_purges() {
  for (;;) {
    PlanId run{};
    {
      const std::scoped_lock lock(mutex_);
      if (purges_.empty()) {
        return;
      }
      run = purges_.front();
    }
    if (Result<std::size_t> purged = executor()->purge_run(run); !purged) {
      note("quarantine run " + std::to_string(run.value()) + ": " + purged.error().message);
    }
    const std::scoped_lock lock(mutex_);
    purges_.erase(purges_.begin());
  }
}

Result<Json> CleaningService::request_purge(PlanId run) {
  if (!settings_.execute) {
    return fail(Errc::invalid_state, "this service does not execute plans");
  }
  Result<Executor*> ready = require_executor();
  if (!ready) {
    return tl::unexpected(ready.error());
  }
  Result<std::vector<actions::QuarantineRecord>> records = fail(Errc::io_error, "not read");
  if (Result<void> read = sources_.store->read([&](store::Database& db) -> Result<void> {
        records = actions::quarantine_records(db, run);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!records) {
    return tl::unexpected(records.error());
  }
  if (std::ranges::none_of(*records, [](const actions::QuarantineRecord& record) {
        return record.state == actions::QuarantineState::held;
      })) {
    return fail(Errc::not_found,
                "quarantine run " + std::to_string(run.value()) + " holds no item");
  }
  {
    const std::scoped_lock lock(mutex_);
    if (std::ranges::find(purges_, run) == purges_.end()) {
      purges_.push_back(run);
    }
  }
  if (wake_) {
    wake_();
  }
  return Json{{"queued", true}};
}

void CleaningService::run_approved() {
  Result<std::vector<PlanId>> approved = executor()->approved_service_plans();
  if (!approved) {
    note("plans: " + approved.error().message);
    return;
  }
  for (const PlanId id : *approved) {
    if (Result<actions::RunReport> ran = executor()->execute(id); !ran) {
      note("plan " + std::to_string(id.value()) + ": " + ran.error().message);
    }
  }
}

void CleaningService::run_scan(bool scheduled, const std::stop_token& stop) {
  {
    const std::scoped_lock lock(mutex_);
    scanning_ = true;
    progress_ = 0.0;
  }
  reload_packs();
  std::optional<clean::Workspaces> workspaces;
  const fs::path workspace_file = sources_.data_dir / "workspaces.toml";
  if (Result<std::string> text = sources_.files->read(workspace_file); text) {
    Result<clean::Workspaces> parsed = clean::parse_workspace_file(*text, workspace_file);
    if (parsed) {
      workspaces = *std::move(parsed);
    } else {
      note(parsed.error().message);
    }
  }
  {
    const std::scoped_lock lock(mutex_);
    progress_ = 0.1;
  }
  const Result<ScanResult> scanned = executor()->scan(
      RuleSet{.rules = registry_.clean, .workspaces = workspaces ? &*workspaces : nullptr},
      settings_.execute, stop);
  Result<void> outcome;
  if (!scanned) {
    note("scan: " + scanned.error().message);
    outcome = tl::unexpected(scanned.error());
  } else {
    for (const std::string& problem : scanned->problems) {
      note(problem);
    }
    if (settings_.execute) {  // every scan's auto-tier plan runs at once
      {
        const std::scoped_lock lock(mutex_);
        progress_ = 0.8;
      }
      // A system volume below 10 % free also gets its hibernation and page file reduced, and
      // the idle content of the automatic recipes moved.
      std::vector<packs::RelocationRecipe> recipes;
      {
        const std::scoped_lock lock(mutex_);
        recipes = recipes_;
      }
      if (const Result<std::vector<PlanId>> reclaimed = executor()->reclaim_system_space(recipes);
          !reclaimed) {
        note("system space: " + reclaimed.error().message);
      }
      run_approved();
    }
  }
  if (scheduled) {
    static_cast<void>(scheduler_.finish(auto_job, outcome));
  }
  const std::scoped_lock lock(mutex_);
  scanning_ = false;
  progress_ = 1.0;
  last_completed_ = sources_.clock->now();
}

Json CleaningService::status() const {
  Result<CandidateTotals> totals = fail(Errc::io_error, "not read");
  static_cast<void>(sources_.store->read([&](store::Database& db) -> Result<void> {
    totals = candidate_totals(db, "scan");
    return {};
  }));
  Json next = nullptr;
  if (settings_.execute) {
    // The repository reads the store only; the scheduler's own copy is the same row.
    sched::StoreScheduleRepository rows(*sources_.store);
    const Result<std::optional<sched::ScheduleState>> row = rows.load(auto_job);
    if (const std::optional<sched::ScheduleState> state = row ? *row : std::nullopt; state) {
      next = store::to_unix_seconds(state->next_run);
    }
  }
  const std::optional<std::string> reason = unavailable();
  const Executor* ready = executor();
  const Json applying = applying_json(ready != nullptr ? ready->applying() : std::nullopt);
  const std::scoped_lock lock(mutex_);
  double progress = last_completed_ ? 1.0 : 0.0;
  if (scanning_) {
    progress = progress_;
  }
  Json purging = Json::array();
  for (const PlanId run : purges_) {
    purging.push_back(run.value());
  }
  return Json{{"state", scanning_ || scan_requested_ ? "scanning" : "idle"},
              {"progress", progress},
              {"last_completed",
               last_completed_ ? Json(store::to_unix_seconds(*last_completed_)) : Json(nullptr)},
              {"candidates", totals ? totals->count : 0},
              {"bytes", totals ? totals->bytes.count() : 0},
              {"next_auto_run", next},
              {"purging", purging},
              {"applying", applying},
              {"unavailable", reason ? Json(*reason) : Json(nullptr)}};
}

Result<Json> CleaningService::approve(const Json& params) {
  const PlanId id = plan_param(params);
  const Json& selection = params.at("selection");
  Result<Plan> plan = fail(Errc::not_found, "not read");
  if (Result<void> read = sources_.store->read([&](store::Database& db) -> Result<void> {
        plan = store::get_plan(db, id);
        return {};
      });
      !read) {
    return tl::unexpected(read.error());
  }
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  std::vector<PlanItemId> approved;
  if (selection.at("all").get<bool>()) {
    const std::vector<std::int64_t> except = ids_of(selection, "except");
    for (const std::int64_t item : except) {
      if (std::ranges::find(plan->items, PlanItemId{item}, &PlanItem::id) == plan->items.end()) {
        return fail(Errc::invalid_argument,
                    "plan " + std::to_string(id.value()) + " has no item " + std::to_string(item));
      }
    }
    for (const PlanItem& item : plan->items) {
      if (std::ranges::find(except, item.id.value()) == except.end()) {
        approved.push_back(item.id);
      }
    }
  } else {
    for (const std::int64_t item : ids_of(selection, "items")) {
      approved.emplace_back(item);
    }
  }
  Result<PlanState> state = engine_.approve(id, approved, Actor::owner());
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (wake_) {
    wake_();
  }
  return Json{{"state", to_string(*state)}};
}

Result<Json> CleaningService::undo(const Json& params) {
  const PlanId id = plan_param(params);
  Result<Plan> plan = fail(Errc::not_found, "not read");
  Result<PlanSpec> spec = fail(Errc::not_found, "not read");
  if (Result<void> read = sources_.store->read([&](store::Database& db) -> Result<void> {
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
  if (spec->scope == ExecutionContext::user) {
    Result<PlanState> state = tasks_.queue_undo(id);
    if (!state) {
      return tl::unexpected(state.error());
    }
    return Json{{"state", to_string(*state)}, {"queued", true}};
  }
  Result<Executor*> ready = require_executor();
  if (!ready) {
    return tl::unexpected(ready.error());
  }
  Result<PlanState> state = (*ready)->undo(id);
  if (!state) {
    return tl::unexpected(state.error());
  }
  return Json{{"state", to_string(*state)}, {"queued", false}};
}

Result<Json> CleaningService::propose_optimization(const Json& params, const Actor& proposer) {
  Result<Executor*> ready = require_executor();
  if (!ready) {
    return tl::unexpected(ready.error());
  }
  const std::string pack = text_param(params, "pack");
  const std::string case_id = text_param(params, "recipe");
  std::optional<packs::RelocationRecipe> found;
  if (!pack.empty() || !case_id.empty()) {
    found = recipe(pack + "/" + case_id);
    if (!found) {
      return fail(Errc::invalid_argument,
                  "no relocation recipe \"" + pack + "/" + case_id + "\" is in effect");
    }
  }
  const auto maximum = params.find("maximum_gb");
  Result<Plan> plan = (*ready)->propose_optimization(
      Optimization{.kind = text_param(params, "kind"),
                   .mode = text_param(params, "mode"),
                   .target = text_param(params, "target"),
                   .volume = text_param(params, "volume"),
                   .maximum_gb = maximum == params.end() ? std::nullopt
                                                         : std::optional(maximum->get<double>()),
                   .cache = text_param(params, "cache"),
                   .content = text_param(params, "content"),
                   .recipe = std::move(found),
                   .instance = text_param(params, "instance")},
      proposer);
  if (!plan) {
    return tl::unexpected(plan.error());
  }
  return Json{{"plan", plan->id.value()}, {"state", to_string(plan->state)}};
}

// How many of the latest logons the impact shown in startup.list is the median of.
constexpr std::size_t startup_logons = 10;

Result<Json> CleaningService::startup_list() {
  if (sources_.probes.startup == nullptr) {
    return fail(Errc::invalid_state, "the startup items cannot be read here");
  }
  Result<startup::Snapshot> snapshot = sources_.probes.startup->read();
  if (!snapshot) {
    return tl::unexpected(snapshot.error());
  }
  const std::vector<startup::Item> items = startup::inventory(*snapshot);
  // The impact of the latest logons the startup component measured; none before the first.
  Result<std::vector<std::vector<startup::ImageImpact>>> history =
      startup::logon_history(*sources_.store, startup_logons);
  // Until then, the boot delays Windows attributed to programs in the last 30 days.
  std::vector<startup::BootDelay> delays;
  if (sources_.diagnostics != nullptr) {
    if (const Result<std::vector<std::string>> events = sources_.diagnostics->events(); events) {
      delays = startup::boot_delays(*events, sources_.clock->now() - startup::delay_span);
    }
  }
  const std::vector<std::optional<startup::ItemImpact>> impacts = startup::item_impacts(
      items, history ? *history : std::vector<std::vector<startup::ImageImpact>>{}, delays);
  Json listed = Json::array();
  std::vector<std::string> ids;
  ids.reserve(items.size());
  for (std::size_t index = 0; index < items.size(); ++index) {
    const startup::Item& item = items[index];
    ids.push_back(item.id);
    Json entry{{"id", static_cast<std::int64_t>(ids.size())},
               {"key", item.id},
               {"source", startup::to_string(item.source)},
               {"scope", startup::to_string(item.scope)},
               {"name", item.name},
               {"display_name", item.display_name},
               {"command", item.command},
               {"enabled", item.enabled ? Json(*item.enabled) : Json(nullptr)}};
    if (item.signer) {
      entry["signer"] = *item.signer;
    }
    if (item.start) {
      entry["start"] = startup::to_string(item.start->type);
      entry["delayed"] = item.start->delayed;
    }
    entry["impact"] = nullptr;
    if (const std::optional<startup::ItemImpact>& impact = impacts[index]) {
      entry["impact"] = Json{{"source", startup::to_string(impact->source)},
                             {"cpu_seconds", impact->cpu_seconds},
                             {"io_bytes", impact->io_bytes},
                             {"logons", impact->logons},
                             {"delay_seconds", impact->delay_seconds}};
    }
    listed.push_back(std::move(entry));
  }
  {
    const std::scoped_lock lock(mutex_);
    startup_items_ = std::move(ids);
  }
  return Json{{"items", std::move(listed)}};
}

// How many contributors startup.boot_times lists.
constexpr std::size_t boot_contributors = 10;

namespace {

double seconds_of(std::chrono::milliseconds duration) {
  return std::chrono::duration<double>(duration).count();
}

Json item_json(startup::Source source, startup::Scope scope, std::string_view name) {
  return Json{
      {"name", name}, {"source", startup::to_string(source)}, {"scope", startup::to_string(scope)}};
}

// {"logon_seconds", "boots"} of the median logon of `boots`, or null when there is none.
Json logon_median(std::span<const startup::BootRecord> boots) {
  const std::optional<startup::DurationMedian> median = startup::median_of(boots);
  return median ? Json{{"logon_seconds", seconds_of(median->logon)}, {"boots", median->boots}}
                : Json(nullptr);
}

// The contributors of a logon: the programs of `impact` that used the most, each with the item
// of `items` that runs it.
Json contributors_json(std::vector<startup::ImageImpact> impact,
                       std::span<const startup::Item> items) {
  std::vector<startup::ImageImpact> ordered = startup::by_impact(std::move(impact));
  ordered.resize(std::min(ordered.size(), boot_contributors));
  Json contributors = Json::array();
  for (const startup::ImageImpact& image : ordered) {
    const auto item = std::ranges::find_if(items, [&](const startup::Item& candidate) {
      return startup::command_image(candidate.command) == image.image;
    });
    contributors.push_back(
        Json{{"image", image.image},
             {"cpu_seconds", image.cpu_seconds},
             {"io_bytes", image.io_bytes},
             {"item", item == items.end() ? Json(nullptr)
                                          : item_json(item->source, item->scope, item->name)}});
  }
  return contributors;
}

// The startup changes of `changes` that disabled items, with the median logon of the boots
// (oldest first) before and after each.
Json changes_json(std::span<const StartupChange> changes,
                  std::span<const startup::BootRecord> boots) {
  Json listed = Json::array();
  for (const StartupChange& change : changes) {
    if (change.enable) {
      continue;
    }
    Json items = Json::array();
    for (const std::string& id : change.items) {
      if (const Result<startup::ItemKey> key = startup::parse_item_id(id); key) {
        items.push_back(item_json(key->source, key->scope, key->name));
      }
    }
    listed.push_back(
        Json{{"plan", change.plan.value()},
             {"applied", store::to_unix_seconds(change.applied)},
             {"items", std::move(items)},
             {"before",
              logon_median(startup::boots_before(boots, change.applied, startup::change_boots))},
             {"after",
              logon_median(startup::boots_after(boots, change.applied, startup::change_boots))}});
  }
  return listed;
}

}  // namespace

std::vector<startup::Item> CleaningService::startup_items() const {
  if (sources_.probes.startup == nullptr) {
    return {};
  }
  const Result<startup::Snapshot> snapshot = sources_.probes.startup->read();
  return snapshot ? startup::inventory(*snapshot) : std::vector<startup::Item>{};
}

Result<Json> CleaningService::latest_boot(
    std::span<const startup::BootRecord> boots,
    const std::optional<startup::DurationMedian>& median) const {
  const startup::BootRecord& latest = boots.back();
  Json entry{{"start", store::to_unix_seconds(latest.start)},
             {"boot_seconds", seconds_of(latest.boot)},
             {"main_path_seconds", seconds_of(latest.main_path)},
             {"post_boot_seconds", seconds_of(latest.post_boot)},
             {"logon_seconds", seconds_of(latest.logon)},
             {"boot_difference_seconds", nullptr},
             {"logon_difference_seconds", nullptr},
             {"logon", nullptr},
             {"contributors", Json::array()}};
  if (median) {
    entry["boot_difference_seconds"] = seconds_of(latest.boot - median->boot);
    entry["logon_difference_seconds"] = seconds_of(latest.logon - median->logon);
  }
  const Result<std::vector<WallTime>> logons = startup::stored_logons(*sources_.store);
  if (!logons) {
    return tl::unexpected(logons.error());
  }
  // What each program used in the first 2 minutes of that logon, if MyClean measured it.
  if (const std::optional<WallTime> logon = startup::boot_logon(boots, boots.size() - 1, *logons)) {
    entry["logon"] = store::to_unix_seconds(*logon);
    Result<std::vector<startup::ImageImpact>> impact =
        startup::stored_logon_impact(*sources_.store, *logon);
    if (!impact) {
      return tl::unexpected(impact.error());
    }
    entry["contributors"] = contributors_json(*std::move(impact), startup_items());
  }
  return entry;
}

Result<Json> CleaningService::boot_times() const {
  Json answer{{"source", "diagnostics"},
              {"log_error", nullptr},
              {"latest", nullptr},
              {"median", nullptr},
              {"changes", Json::array()}};
  // The boots Windows measured since the startup component last recorded them.
  if (sources_.diagnostics != nullptr) {
    if (const Result<void> recorded =
            startup::record_boot_events(*sources_.store, *sources_.diagnostics);
        !recorded) {
      answer["log_error"] = recorded.error().message;
    }
  }
  const Result<std::vector<startup::BootRecord>> boots = startup::stored_boots(*sources_.store);
  if (!boots) {
    return tl::unexpected(boots.error());
  }
  const auto recent = std::ranges::lower_bound(*boots, sources_.clock->now() - startup::median_span,
                                               {}, &startup::BootRecord::start);
  const std::optional<startup::DurationMedian> median =
      startup::median_of(std::span<const startup::BootRecord>(recent, boots->end()));
  if (median) {
    answer["median"] = Json{{"boot_seconds", seconds_of(median->boot)},
                            {"logon_seconds", seconds_of(median->logon)},
                            {"boots", median->boots}};
  }
  if (!boots->empty()) {
    Result<Json> latest = latest_boot(*boots, median);
    if (!latest) {
      return tl::unexpected(latest.error());
    }
    answer["latest"] = *std::move(latest);
  }
  // The logons before and after each startup item disabled through an approved plan.
  const Result<std::vector<StartupChange>> changes = startup_changes(*sources_.store);
  if (!changes) {
    return tl::unexpected(changes.error());
  }
  answer["changes"] = changes_json(*changes, *boots);
  return answer;
}

Result<Json> CleaningService::propose_startup(const Json& params, const Actor& proposer) {
  std::vector<std::string> chosen;
  {
    const std::scoped_lock lock(mutex_);
    for (const std::int64_t id : ids_of(params.at("candidates"), "ids")) {
      if (id < 1 || std::cmp_greater(id, startup_items_.size())) {
        return fail(Errc::not_found,
                    "no startup item " + std::to_string(id) + " in the latest startup.list answer");
      }
      chosen.push_back(startup_items_[static_cast<std::size_t>(id - 1)]);
    }
  }
  const bool enable = params.value("enable", false);
  return on_executor([&](Executor& executor) {
    return plan_answer(executor.propose_startup(chosen, enable, proposer));
  });
}

Result<Json> CleaningService::space_answer(const std::string& key, bool refresh) {
  const WallTime now = sources_.clock->now();
  bool queued = false;
  Json answer;
  {
    const std::scoped_lock lock(mutex_);
    SpaceEntry& entry = space_[key];
    const bool stale = !entry.scanned_at ||
                       now - *entry.scanned_at > std::chrono::minutes{ipc::space_result_minutes};
    if (!entry.running && (refresh || stale)) {
      entry.running = true;
      space_queue_.push_back(key);
      queued = true;
    }
    answer = entry.result;
    std::string_view state = entry.error ? "failed" : "ready";
    if (entry.running) {
      state = "scanning";
    }
    answer["state"] = state;
    answer["scanned_at"] =
        entry.scanned_at ? Json(store::to_unix_seconds(*entry.scanned_at)) : Json(nullptr);
    answer["error"] = entry.error ? Json(*entry.error) : Json(nullptr);
  }
  if (queued && wake_) {
    wake_();
  }
  return answer;
}

void CleaningService::work_space(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    std::string key;
    {
      const std::scoped_lock lock(mutex_);
      if (space_queue_.empty()) {
        return;
      }
      key = std::move(space_queue_.front());
      space_queue_.pop_front();
    }
    Result<Json> result = analyze(key, stop);
    const std::scoped_lock lock(mutex_);
    SpaceEntry& entry = space_[key];
    entry.running = false;
    entry.scanned_at = sources_.clock->now();
    if (result) {
      entry.result = *std::move(result);
      entry.error.reset();
    } else {
      entry.error = result.error().message;
    }
  }
}

Result<space::ScanResult> CleaningService::scan_folder(const fs::path& root,
                                                       const std::stop_token& stop) const {
  return space::scan(*sources_.space.reader, root, {}, stop);
}

Result<Json> CleaningService::analyze(const std::string& key, const std::stop_token& stop) {
  if (sources_.space.reader == nullptr) {
    return fail(Errc::invalid_state, "the space analyses are not available");
  }
  if (executor() == nullptr) {
    return fail(Errc::invalid_state, "the owner's folders are unknown until the owner logs on");
  }
  if (key.starts_with("top:")) {
    return analyze_top(key, stop);
  }
  if (key == "duplicates") {
    return analyze_duplicates(stop);
  }
  if (key == "photo_duplicates") {
    return analyze_photo_duplicates(stop);
  }
  if (key == "owner_decisions") {
    return analyze_decisions(stop);
  }
  if (key == "indexed_folders") {
    return analyze_indexed_folders(stop);
  }
  const std::optional<fs::path> downloads = owner_->downloads;
  if (!downloads) {
    return fail(Errc::not_found, "the owner's Downloads folder is unknown");
  }
  if (key == "old_downloads") {
    return analyze_old_downloads(*downloads, stop);
  }
  if (key == "used_installers") {
    return analyze_installers(*downloads, stop);
  }
  return fail(Errc::invalid_argument, "unknown analysis " + key);
}

Result<Json> CleaningService::analyze_top(const std::string& key, const std::stop_token& stop) {
  const std::string where = key.substr(4);
  fs::path root;
  if (where == "downloads") {
    const std::optional<fs::path> downloads = owner_->downloads;
    if (!downloads) {
      return fail(Errc::not_found, "the owner's Downloads folder is unknown");
    }
    root = *downloads;
  } else if (where == "profile") {
    root = owner_->locations.owner_profile;
  } else {
    root = fs::path(std::wstring{static_cast<wchar_t>(where.front()), L':', L'\\'});
  }
  Result<space::ScanResult> scanned = scan_folder(root, stop);
  if (!scanned) {
    return tl::unexpected(scanned.error());
  }
  const space::TopItems top =
      space::top_items(*scanned, static_cast<std::size_t>(ipc::max_space_top));
  std::vector<Candidate> found;
  found.reserve(top.files.size());
  const WallTime now = sources_.clock->now();
  for (const space::TopFile& file : top.files) {
    found.push_back(file_candidate(file.path, file.size_on_disk, "one of the largest files", now));
  }
  if (Result<void> stored = store_candidates(*sources_.store, "space." + key, found); !stored) {
    return tl::unexpected(stored.error());
  }
  Json json = top;
  for (std::size_t index = 0; index < found.size(); ++index) {
    json["files"][index]["id"] = found[index].id;
    if (const std::optional<std::string_view> denied =
            safety::denied_by(found[index].path, owner_->locations)) {
      json["files"][index]["protected"] = std::string(*denied);
    }
  }
  json["path"] = utf8(root);
  return json;
}

Result<Json> CleaningService::analyze_old_downloads(const fs::path& downloads,
                                                    const std::stop_token& stop) {
  if (sources_.space.zones == nullptr) {
    return fail(Errc::invalid_state, "the Zone.Identifier reader is not available");
  }
  Result<space::ScanResult> scanned = scan_folder(downloads, stop);
  if (!scanned) {
    return tl::unexpected(scanned.error());
  }
  const WallTime now = sources_.clock->now();
  std::vector<Candidate> found;
  Json files = Json::array();
  for (const space::OldDownload& download :
       space::old_downloads(*scanned, now, *sources_.space.zones)) {
    found.push_back(file_candidate(
        download.path, download.size_on_disk,
        "download untouched for " + std::to_string(download.age_days) + " days", now));
    files.push_back(download);
  }
  if (Result<void> stored = store_candidates(*sources_.store, "space.old_downloads", found);
      !stored) {
    return tl::unexpected(stored.error());
  }
  for (std::size_t index = 0; index < found.size(); ++index) {
    files[index]["id"] = found[index].id;
  }
  return Json{{"folder", utf8(downloads)}, {"files", std::move(files)}};
}

Result<Json> CleaningService::analyze_installers(const fs::path& downloads,
                                                 const std::stop_token& stop) {
  if (sources_.space.installers == nullptr) {
    return fail(Errc::invalid_state, "the installer inspector is not available");
  }
  Result<space::ScanResult> scanned = scan_folder(downloads, stop);
  if (!scanned) {
    return tl::unexpected(scanned.error());
  }
  Result<space::InstallerReport> report =
      space::used_installers(*scanned, *sources_.space.installers);
  if (!report) {
    return tl::unexpected(report.error());
  }
  const WallTime now = sources_.clock->now();
  std::vector<Candidate> found;
  for (const space::UsedInstaller& used : report->used) {
    found.push_back(file_candidate(used.path, used.size_on_disk,
                                   "installer of " + used.installed_as + ", already installed",
                                   now));
  }
  if (Result<void> stored = store_candidates(*sources_.store, "space.used_installers", found);
      !stored) {
    return tl::unexpected(stored.error());
  }
  Json json = *report;
  for (std::size_t index = 0; index < found.size(); ++index) {
    json["used"][index]["id"] = found[index].id;
  }
  json["folder"] = utf8(downloads);
  return json;
}

std::vector<space::ScanResult> CleaningService::scan_owner_folders(const std::stop_token& stop,
                                                                   Json& roots_json) const {
  std::vector<fs::path> roots = owner_->locations.owner_known_folders;
  if (const std::optional<fs::path> downloads = owner_->downloads; downloads) {
    roots.push_back(*downloads);
  }
  std::vector<space::ScanResult> scans;
  roots_json = Json::array();
  for (const fs::path& root : roots) {
    if (Result<space::ScanResult> scanned = scan_folder(root, stop); scanned) {
      scans.push_back(*std::move(scanned));
      roots_json.push_back(utf8(root));
    }
  }
  return scans;
}

Result<Json> CleaningService::analyze_duplicates(const std::stop_token& stop) {
  if (sources_.space.hasher == nullptr) {
    return fail(Errc::invalid_state, "the content hasher is not available");
  }
  Json roots_json;
  const std::vector<space::ScanResult> scans = scan_owner_folders(stop, roots_json);
  const space::DuplicateReport report =
      space::find_duplicates(scans, *sources_.space.hasher, {}, stop);
  const WallTime now = sources_.clock->now();
  std::vector<Candidate> found;
  for (const space::DuplicateGroup& group : report.groups) {
    for (const space::DuplicateFile& file : group.files) {
      found.push_back(file_candidate(
          file.path, file.size_on_disk,
          "exact duplicate (" + std::to_string(group.files.size()) + " copies)", now));
    }
  }
  if (Result<void> stored = store_candidates(*sources_.store, "space.duplicates", found); !stored) {
    return tl::unexpected(stored.error());
  }
  Json json = report;
  std::size_t next = 0;
  for (Json& group : json["groups"]) {
    for (Json& file : group["files"]) {
      file["id"] = found[next++].id;
    }
  }
  json["roots"] = std::move(roots_json);
  json["mode"] = "exact";
  fit_groups(json);  // the most reclaimable groups first
  return json;
}

Result<Json> CleaningService::analyze_photo_duplicates(const std::stop_token& stop) {
  if (sources_.space.images == nullptr) {
    return fail(Errc::invalid_state, "the image decoder is not available");
  }
  Json roots_json;
  const std::vector<space::ScanResult> scans = scan_owner_folders(stop, roots_json);
  const space::PhotoReport report =
      space::find_similar_photos(scans, *sources_.space.images, {}, stop);
  const WallTime now = sources_.clock->now();
  std::vector<Candidate> found;
  for (const space::PhotoGroup& group : report.groups) {
    for (std::size_t index = 0; index < group.photos.size(); ++index) {
      const space::Photo& photo = group.photos[index];
      found.push_back(file_candidate(
          photo.path, photo.size_on_disk,
          std::string(index == 0 ? "the copy to keep of " : "near-duplicate photo of ") +
              std::to_string(group.photos.size()) + " similar photos",
          now));
    }
  }
  if (Result<void> stored = store_candidates(*sources_.store, "space.photo_duplicates", found);
      !stored) {
    return tl::unexpected(stored.error());
  }
  Json json = report;
  std::size_t next = 0;
  for (Json& group : json["groups"]) {
    for (Json& file : group["files"]) {
      file["id"] = found[next++].id;
    }
  }
  json["roots"] = std::move(roots_json);
  json["mode"] = "photos";
  fit_groups(json);  // the most reclaimable groups first
  return json;
}

Result<Json> CleaningService::analyze_decisions(const std::stop_token& stop) {
  const fs::path profile = owner_->locations.owner_profile;
  if (profile.empty()) {
    return fail(Errc::not_found, "the owner's profile folder is unknown");
  }
  Result<space::ScanResult> scanned = scan_folder(profile, stop);
  if (!scanned) {
    return tl::unexpected(scanned.error());
  }
  const WallTime now = sources_.clock->now();
  Json notes = Json::array();
  std::vector<Candidate> found;
  Json cold = Json::array();
  for (const scanall::Finding& finding : cold_folders(*scanned, profile, now, notes)) {
    const std::string* target = finding.plan.param("target");
    found.push_back(Candidate{.source = std::string(owner_decisions_source),
                              .category = "space",
                              .tier = Tier::ask,
                              .scope = ExecutionContext::user,
                              .strategy = "archive",
                              .path = finding.path,
                              .size = finding.bytes,
                              .reason = finding.reason,
                              .found_at = now});
    Json row{{"path", utf8(finding.path)},
             {"bytes", finding.bytes.count()},
             {"target", target != nullptr ? Json(*target) : Json(nullptr)},
             {"reason", finding.reason}};
    if (const std::optional<std::string_view> denied =
            safety::denied_by(finding.path, owner_->locations)) {
      row["protected"] = std::string(*denied);
    }
    cold.push_back(std::move(row));
  }
  Json programs = Json::array();
  for (const scanall::ProgramFinding& finding : review_programs(notes, stop)) {
    const scanall::UninstallEntry& program = finding.program;
    const std::string reason = finding.area + (finding.note.empty() ? "" : ": " + finding.note);
    found.push_back(Candidate{.source = std::string(owner_decisions_source),
                              .category = "space",
                              .tier = Tier::ask,
                              .scope = ExecutionContext::user,
                              .strategy = "uninstall",
                              .path = program.location,
                              .size = program.estimated,
                              .command = program.key,
                              .handler = program.name,
                              .reason = reason,
                              .found_at = now});
    programs.push_back(Json{{"program", program.name},
                            {"publisher", program.publisher},
                            {"version", program.version},
                            {"location", utf8(program.location)},
                            {"bytes", program.estimated.count()},
                            {"area", finding.area},
                            {"reason", reason}});
  }
  if (Result<void> stored =
          store_candidates(*sources_.store, std::string(owner_decisions_source), found);
      !stored) {
    return tl::unexpected(stored.error());
  }
  for (std::size_t index = 0; index < found.size(); ++index) {
    Json& row = index < cold.size() ? cold[index] : programs[index - cold.size()];
    row["id"] = found[index].id;
  }
  return Json{{"profile", utf8(profile)},
              {"cold", std::move(cold)},
              {"uninstall", std::move(programs)},
              {"notes", std::move(notes)}};
}

Result<Json> CleaningService::analyze_indexed_folders(const std::stop_token& stop) {
  if (sources_.probes.indexer == nullptr || sources_.probes.tree == nullptr) {
    return fail(Errc::invalid_state, "the Windows Search scope cannot be read here");
  }
  std::vector<map::Area> areas;
  std::vector<rules::CleaningRule> rules;
  {
    const std::scoped_lock lock(mutex_);
    areas = index_areas_;
    rules = project_rules_;
  }
  Result<std::optional<clean::Workspaces>> workspaces =
      read_workspaces(*sources_.files, sources_.data_dir);
  if (!workspaces) {
    return tl::unexpected(workspaces.error());
  }
  Result<std::vector<indexer::IndexedFolder>> folders = indexer::indexed_build_folders(
      *sources_.probes.indexer, *sources_.probes.tree, *owner_->environment,
      indexer::Knowledge{
          .areas = areas, .rules = rules, .workspaces = *workspaces ? &**workspaces : nullptr},
      stop);
  if (!folders) {
    return tl::unexpected(folders.error());
  }
  const WallTime now = sources_.clock->now();
  std::vector<Candidate> found;
  found.reserve(folders->size());
  for (const indexer::IndexedFolder& folder : *folders) {
    found.push_back(Candidate{.source = std::string(indexed_folders_source),
                              .category = folder.category,
                              .tier = Tier::ask,
                              .scope = ExecutionContext::service,
                              .strategy = "index_exclude",
                              .path = folder.path,
                              .reason = folder.label,
                              .found_at = now});
  }
  if (Result<void> stored =
          store_candidates(*sources_.store, std::string(indexed_folders_source), found);
      !stored) {
    return tl::unexpected(stored.error());
  }
  Json list = Json::array();
  for (const Candidate& candidate : found) {
    list.push_back(Json{{"id", candidate.id},
                        {"path", utf8(candidate.path)},
                        {"category", candidate.category},
                        {"label", candidate.reason}});
  }
  return Json{{"folders", std::move(list)}};
}

std::vector<scanall::Finding> CleaningService::cold_folders(const space::ScanResult& tree,
                                                            const fs::path& profile, WallTime now,
                                                            Json& notes) const {
  std::vector<scanall::DriveInfo> drives;
  if (sources_.probes.drives == nullptr) {
    notes.push_back("the drives cannot be read here, so no folder has an archive target");
  } else if (Result<std::vector<placement::TargetVolume>> volumes =
                 sources_.probes.drives->volumes();
             volumes) {
    drives = scanall::drive_infos(*volumes);
  } else {
    notes.push_back("drives: " + volumes.error().message);
  }
  const scanall::DriveInfo* drive = scanall::drive_of(drives, profile);
  if (drive == nullptr) {
    notes.push_back("the drive of " + utf8(profile) + " is unknown, so no folder is judged cold");
    return {};
  }
  const std::vector<fs::path> in_use = sources_.probes.usage != nullptr
                                           ? sources_.probes.usage->process_folders()
                                           : std::vector<fs::path>{};
  return scanall::find_cold(tree, scanall::folder_times(tree), *drive, drives, in_use, now);
}

std::vector<scanall::ProgramFinding> CleaningService::review_programs(
    Json& notes, const std::stop_token& stop) const {
  const placement::IRegistry* registry =
      owner_->tools != nullptr ? owner_->tools->registry : nullptr;
  if (registry == nullptr) {
    notes.push_back("the owner's registry cannot be read here, so no program is listed");
    return {};
  }
  Result<std::vector<scanall::UninstallEntry>> entries = scanall::uninstall_entries(*registry);
  if (!entries) {
    notes.push_back("installed programs: " + entries.error().message);
    return {};
  }
  std::vector<map::Area> areas;
  {
    const std::scoped_lock lock(mutex_);
    areas = uninstall_areas_;
  }
  std::vector<scanall::ProgramFinding> review =
      scanall::uninstall_review(*entries, areas, *owner_->environment);
  for (scanall::ProgramFinding& finding : review) {
    if (stop.stop_requested()) {
      break;
    }
    if (finding.program.estimated.count() == 0) {
      finding.program.estimated =
          placement::readable_size(*sources_.file_system, finding.program.location)
              .value_or(Bytes{});
    }
  }
  std::ranges::sort(review,
                    [](const scanall::ProgramFinding& left, const scanall::ProgramFinding& right) {
                      return left.program.estimated > right.program.estimated;
                    });
  if (review.size() > ipc::max_owner_programs) {
    review.resize(ipc::max_owner_programs);
  }
  return review;
}

Json CleaningService::request_scan() {
  if (executor() == nullptr) {
    return Json{{"started", false}};
  }
  bool started = false;
  {
    const std::scoped_lock lock(mutex_);
    started = !scanning_ && !scan_requested_;
    scan_requested_ = true;
  }
  if (started && wake_) {
    wake_();
  }
  return Json{{"started", started}};
}

Result<Json> CleaningService::space_top(const Json& params) {
  std::string where = params.at("path").get<std::string>();
  if (where.size() == 1 && std::isalpha(static_cast<unsigned char>(where[0])) != 0) {
    where[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(where[0])));
  } else if (where != "downloads" && where != "profile") {
    return fail(Errc::invalid_argument, R"(path is a drive letter, "downloads" or "profile")");
  }
  const std::int64_t count = params.value("n", std::int64_t{20});
  if (count < 1 || count > ipc::max_space_top) {
    return fail(Errc::invalid_argument, "n must be 1..100");
  }
  Result<Json> answer = space_answer("top:" + where, params.value("refresh", false));
  for (const char* list : {"files", "folders"}) {
    if (answer && answer->contains(list) &&
        (*answer)[list].size() > static_cast<std::size_t>(count)) {
      (*answer)[list].erase((*answer)[list].begin() + count, (*answer)[list].end());
    }
  }
  return answer;
}

Result<Json> CleaningService::on_executor(
    const std::function<Result<Json>(Executor& executor)>& work) const {
  Result<Executor*> ready = require_executor();
  if (!ready) {
    return tl::unexpected(ready.error());
  }
  return work(**ready);
}

Result<void> CleaningService::register_methods(ipc::Dispatcher& methods) {
  const auto add = [&](std::string_view name, std::vector<Param> params, ipc::Handler handler,
                       Access access = Access::any) {
    return methods.add(std::string(name), std::move(params), std::move(handler), access);
  };
  const Param id{.name = "id", .type = ParamType::integer, .required = true};
  const Param refresh{.name = "refresh", .type = ParamType::boolean};
  const Param client{.name = "client", .type = ParamType::token};
  const Param candidates{.name = "candidates",
                         .type = ParamType::record,
                         .required = true,
                         .schema = &ipc::check_candidates};
  store::Store& store = *sources_.store;

  std::vector<Result<void>> added;
  added.push_back(add(ipc::cleaning_scan_method, {},
                      [this](const Json&) -> Result<Json> { return request_scan(); }));
  added.push_back(add(ipc::cleaning_status_method, {},
                      [this](const Json&) -> Result<Json> { return status(); }));
  added.push_back(add(ipc::plans_list_method, {Param{.name = "state", .type = ParamType::token}},
                      [&store](const Json& params) -> Result<Json> {
                        const std::optional<PlanState> state =
                            parse_plan_state(params.value("state", std::string{}));
                        if (params.contains("state") && !state) {
                          return fail(Errc::invalid_argument, "unknown plan state");
                        }
                        return list_plans(store, state);
                      }));
  added.push_back(add(ipc::plans_get_method, {id}, [&store](const Json& params) {
    return get_plan(store, plan_param(params));
  }));
  added.push_back(add(
      ipc::plans_approve_method,
      {id, Param{.name = "selection",
                 .type = ParamType::record,
                 .required = true,
                 .schema = &ipc::check_selection}},
      [this](const Json& params) { return approve(params); }, Access::tray));
  added.push_back(add(
      ipc::plans_reject_method, {id},
      [this](const Json& params) {
        return state_answer(engine_.approve(plan_param(params), {}, Actor::owner()));
      },
      Access::tray));
  added.push_back(add(
      ipc::plans_undo_method, {id}, [this](const Json& params) { return undo(params); },
      Access::tray));
  added.push_back(methods.add_for_caller(
      std::string(ipc::plans_propose_cleanup_method), {candidates, client},
      [this](const Json& params, const ipc::Caller& caller) {
        return on_executor([&](Executor& executor) {
          return plan_answer(executor.propose_cleanup(ids_of(params.at("candidates"), "ids"),
                                                      proposer_of(params, caller)));
        });
      }));
  added.push_back(
      methods.add_for_caller(std::string(ipc::plans_propose_optimization_method),
                             {Param{.name = "kind", .type = ParamType::token, .required = true},
                              client, Param{.name = "mode", .type = ParamType::token},
                              Param{.name = "target", .type = ParamType::token},
                              Param{.name = "volume", .type = ParamType::token},
                              Param{.name = "maximum_gb", .type = ParamType::number},
                              Param{.name = "cache", .type = ParamType::token},
                              Param{.name = "content", .type = ParamType::token},
                              Param{.name = "pack", .type = ParamType::token},
                              Param{.name = "recipe", .type = ParamType::token},
                              Param{.name = "instance", .type = ParamType::token}},
                             [this](const Json& params, const ipc::Caller& caller) {
                               return propose_optimization(params, proposer_of(params, caller));
                             }));
  added.push_back(
      add(ipc::startup_list_method, {}, [this](const Json& /*params*/) { return startup_list(); }));
  added.push_back(add(ipc::startup_boot_times_method, {},
                      [this](const Json& /*params*/) { return boot_times(); }));
  added.push_back(methods.add_for_caller(
      std::string(ipc::plans_propose_startup_method),
      {candidates, client, Param{.name = "enable", .type = ParamType::boolean}},
      [this](const Json& params, const ipc::Caller& caller) {
        return propose_startup(params, proposer_of(params, caller));
      }));
  added.push_back(methods.add_for_caller(
      std::string(ipc::plans_propose_index_exclusion_method), {candidates, client},
      [this](const Json& params, const ipc::Caller& caller) {
        return on_executor([&](Executor& executor) {
          return plan_answer(executor.propose_index_exclusion(
              ids_of(params.at("candidates"), "ids"), proposer_of(params, caller)));
        });
      }));
  added.push_back(add(
      ipc::plans_recycle_method, {candidates},
      [this](const Json& params) {
        return on_executor([&](Executor& executor) {
          return plan_answer(executor.recycle(ids_of(params.at("candidates"), "ids")));
        });
      },
      Access::tray));
  added.push_back(add(
      ipc::plans_owner_action_method,
      {Param{.name = "candidate", .type = ParamType::integer, .required = true}},
      [this](const Json& params) {
        return on_executor([&](Executor& executor) {
          return plan_answer(executor.owner_action(params.at("candidate").get<std::int64_t>()));
        });
      },
      Access::tray));
  added.push_back(add(
      ipc::user_tasks_next_method, {}, [this](const Json&) { return tasks_.next(); },
      Access::tray));
  added.push_back(add(
      ipc::user_tasks_report_method,
      {id, Param{.name = "result",
                 .type = ParamType::record,
                 .required = true,
                 .schema = &ipc::check_task_result}},
      [this](const Json& params) {
        return state_answer(tasks_.report(plan_param(params), params.at("result")));
      },
      Access::tray));
  added.push_back(add(ipc::quarantine_list_method, {},
                      [&store](const Json&) { return list_quarantine(store); }));
  added.push_back(add(
      ipc::quarantine_restore_method,
      {Param{.name = "plan", .type = ParamType::integer, .required = true},
       Param{.name = "item", .type = ParamType::integer}},
      [this](const Json& params) {
        return on_executor([&](Executor& executor) -> Result<Json> {
          const std::optional<PlanItemId> item =
              params.contains("item")
                  ? std::optional(PlanItemId{params.at("item").get<std::int64_t>()})
                  : std::nullopt;
          Result<std::size_t> restored = executor.restore(plan_param(params, "plan"), item);
          if (!restored) {
            return tl::unexpected(restored.error());
          }
          return Json{{"restored", *restored}};
        });
      },
      Access::tray));
  added.push_back(add(
      ipc::quarantine_purge_method,
      {Param{.name = "plan", .type = ParamType::integer, .required = true}},
      [this](const Json& params) { return request_purge(plan_param(params, "plan")); },
      Access::tray));
  added.push_back(add(ipc::space_top_method,
                      {Param{.name = "path", .type = ParamType::token, .required = true},
                       Param{.name = "n", .type = ParamType::integer}, refresh},
                      [this](const Json& params) { return space_top(params); }));
  added.push_back(
      add(ipc::space_duplicates_method, {refresh, Param{.name = "mode", .type = ParamType::token}},
          [this](const Json& params) -> Result<Json> {
            const std::string mode = params.value("mode", std::string("exact"));
            if (mode != "exact" && mode != "photos") {
              return fail(Errc::invalid_argument, "mode must be exact or photos, not " + mode);
            }
            return space_answer(mode == "photos" ? "photo_duplicates" : "duplicates",
                                params.value("refresh", false));
          }));
  for (const auto& [method, key] :
       {std::pair{ipc::space_old_downloads_method, "old_downloads"},
        std::pair{ipc::space_used_installers_method, "used_installers"},
        std::pair{ipc::space_owner_decisions_method, "owner_decisions"},
        std::pair{ipc::space_indexed_folders_method, "indexed_folders"}}) {
    added.push_back(add(method, {refresh}, [this, key](const Json& params) {
      return space_answer(key, params.value("refresh", false));
    }));
  }
  const auto failed =
      std::ranges::find_if(added, [](const Result<void>& result) { return !result; });
  return failed == added.end() ? Result<void>{} : *failed;
}

}  // namespace myclean::cleaning
