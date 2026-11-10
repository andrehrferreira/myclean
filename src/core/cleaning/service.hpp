#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/actions/store_plan_source.hpp"
#include "core/clean/probes.hpp"
#include "core/cleaning/executor.hpp"
#include "core/cleaning/tray_tasks.hpp"
#include "core/ipc/json_rpc.hpp"
#include "core/ipc/status.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/file_system.hpp"
#include "core/packs/applies.hpp"
#include "core/packs/loader.hpp"
#include "core/packs/registry.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/relocation.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/owner_decisions.hpp"
#include "core/sched/scheduler.hpp"
#include "core/sched/store_schedule_repository.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/installers.hpp"
#include "core/space/photos.hpp"
#include "core/space/scanner.hpp"
#include "core/startup/boot_times.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/startup.hpp"
#include "core/store/store.hpp"

// Cleaning in the service (cleaning spec C4, C5; approvals spec; space spec, prompt 3): the
// recurring auto run and the quarantine purge on MyClean's own scheduler, on-demand scans,
// execution of approved service-scope plans, the user tasks of the tray, the read-only space
// analyses, and every method of core/ipc/plans.hpp. It owns no thread: the service component
// calls work() and work_space() from two threads of its own.
namespace myclean::cleaning {

// What only the owner's logon session tells: the owner's environment variables (rule targets
// such as %TEMP%), the deny-list locations of the owner and the Downloads folder.
struct OwnerContext {
  std::unique_ptr<clean::IEnvironment> environment;
  safety::ProtectedLocations locations;
  std::optional<std::filesystem::path> downloads;  // nullopt: unknown
  // git as the owner, who owns the repositories (git refuses another user's); null: Probes::git,
  // when this process runs as the owner.
  std::unique_ptr<clean::IGit> git;
  // The owner's user variables, registry hive and running programs: where automatic relocation
  // recipes are now (Probes::owner_tools). Null: none is moved automatically.
  std::shared_ptr<const placement::Tools> tools;
};
// Fails while the owner's session is unavailable (not logged on); retried every minute.
using OwnerSource = std::function<Result<OwnerContext>()>;

// The read-only probes of the space analyses.
struct SpaceProbes {
  const space::IDirectoryReader* reader = nullptr;
  const space::IZoneIdentifierReader* zones = nullptr;
  const space::IContentHasher* hasher = nullptr;
  const space::IInstallerInspector* installers = nullptr;
  const space::IImageDecoder* images = nullptr;  // near-duplicate photos
};

// Every pointer must outlive the service.
struct ServiceSources {
  store::Store* store = nullptr;  // opened with action_migrations() and cleaning_migrations()
  const os::IClock* clock = nullptr;
  os::IFileSystem* file_system = nullptr;
  Probes probes;  // `environment` is taken from the owner context
  OwnerSource owner;
  const packs::IPackFiles* files = nullptr;
  std::filesystem::path data_dir;  // packs\ (knowledge packs) and workspaces.toml
  // Whether a pack applies (docs/packs.md, applies_when); null: every loaded pack does.
  const packs::IMachine* machine = nullptr;
  // The owner's override packs folder (platform/packs: nullopt when there is none, an error when
  // it is not admin-only); empty: no owner packs.
  std::function<Result<std::optional<std::filesystem::path>>()> owner_packs;
  SpaceProbes space;
  // True while the system volume is critically busy: the auto run waits. Empty: never busy.
  std::function<bool()> busy;
  // Windows' boot measurements: the startup impact shown before MyClean has measured a logon,
  // and the boot and logon durations. Null: none.
  const startup::IDiagnosticsLog* diagnostics = nullptr;
};

struct ServiceSettings {
  ExecutorOptions executor;
  std::string auto_schedule = "0 * * * *";    // the auto run: hourly, with catch-up
  std::string purge_schedule = "15 * * * *";  // the quarantine purge: hourly
  // False: no recurring job and no execution (a console run without a sandbox). Scans,
  // proposals, approvals and the read side still work.
  bool execute = true;
};

class CleaningService {
 public:
  static constexpr std::string_view auto_job = "cleaning.auto";
  static constexpr std::string_view purge_job = "quarantine.purge";
  static constexpr std::chrono::seconds owner_retry{60};

  CleaningService(ServiceSources sources, ServiceSettings settings);
  CleaningService(const CleaningService&) = delete;
  CleaningService& operator=(const CleaningService&) = delete;
  CleaningService(CleaningService&&) = delete;
  CleaningService& operator=(CleaningService&&) = delete;
  ~CleaningService();

  // Called whenever work is queued; the component wakes its threads. Set before start().
  void on_work(std::function<void()> wake) { wake_ = std::move(wake); }

  // Registers the recurring jobs (when executing) and every method of core/ipc/plans.hpp.
  [[nodiscard]] Result<void> start(ipc::Dispatcher& methods);

  // One pass of the cleaning worker: resolves the owner context while it is missing, starts the
  // due jobs (the auto run waits while the system volume is critically busy), runs a requested
  // scan, then every approved service-scope plan. Never call it concurrently with itself.
  void work(const std::stop_token& stop);
  // One pass of the space worker: runs the queued analyses.
  void work_space(const std::stop_token& stop);

  // Why the component is degraded; empty when it is not.
  [[nodiscard]] std::vector<std::string> problems() const;
  // Whether scans and executions can run, and why not.
  [[nodiscard]] std::optional<std::string> unavailable() const;
  // Every pack folder as last loaded (when the owner's session became available and at every
  // scan); empty before.
  [[nodiscard]] std::vector<ipc::PackStatus> pack_status() const;

 private:
  struct SpaceEntry {
    bool running = false;
    std::optional<WallTime> scanned_at;
    std::optional<std::string> error;
    nlohmann::json result = nlohmann::json::object();
  };

  [[nodiscard]] Result<void> register_methods(ipc::Dispatcher& methods);
  void resolve_owner();
  // Loads the knowledge packs again: the clean cases scans evaluate and the relocation recipes
  // plans may use. The owner context must be resolved.
  void reload_packs();
  // The relocation recipe "<pack>/<case>" of the packs in effect; nullopt when there is none.
  [[nodiscard]] std::optional<packs::RelocationRecipe> recipe(std::string_view id) const;
  // Starts the due jobs: the auto run (unless the system volume is critically busy) and the
  // quarantine purge.
  void run_jobs(const std::stop_token& stop);
  void run_scan(bool scheduled, const std::stop_token& stop);
  void run_approved();
  void note(std::string problem);
  [[nodiscard]] Executor* executor() const { return ready_.load(); }
  [[nodiscard]] Result<Executor*> require_executor() const;

  [[nodiscard]] nlohmann::json status() const;
  [[nodiscard]] Result<nlohmann::json> approve(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> undo(const nlohmann::json& params);
  [[nodiscard]] Result<nlohmann::json> propose_optimization(const nlohmann::json& params,
                                                            const Actor& proposer);
  // startup.list: the inventory read now; remembers the item of each "id".
  [[nodiscard]] Result<nlohmann::json> startup_list();
  // startup.boot_times: the boots and logons Windows measured, with what MyClean knows of them.
  [[nodiscard]] Result<nlohmann::json> boot_times() const;
  // The "latest" member of startup.boot_times: the last of `boots` (oldest first, not empty).
  [[nodiscard]] Result<nlohmann::json> latest_boot(
      std::span<const startup::BootRecord> boots,
      const std::optional<startup::DurationMedian>& median) const;
  // The startup inventory read now; empty without the probe or when it cannot be read.
  [[nodiscard]] std::vector<startup::Item> startup_items() const;
  // plans.propose_startup: the items of the latest startup.list answer named by "id".
  [[nodiscard]] Result<nlohmann::json> propose_startup(const nlohmann::json& params,
                                                       const Actor& proposer);
  [[nodiscard]] Result<nlohmann::json> space_answer(const std::string& key, bool refresh);
  [[nodiscard]] Result<nlohmann::json> analyze(const std::string& key, const std::stop_token& stop);
  [[nodiscard]] Result<space::ScanResult> scan_folder(const std::filesystem::path& root,
                                                      const std::stop_token& stop) const;
  [[nodiscard]] Result<nlohmann::json> analyze_top(const std::string& key,
                                                   const std::stop_token& stop);
  [[nodiscard]] Result<nlohmann::json> analyze_old_downloads(const std::filesystem::path& downloads,
                                                             const std::stop_token& stop);
  [[nodiscard]] Result<nlohmann::json> analyze_installers(const std::filesystem::path& downloads,
                                                          const std::stop_token& stop);
  // The scans of the owner's known folders and Downloads that could be read; their roots go to
  // `roots_json`.
  [[nodiscard]] std::vector<space::ScanResult> scan_owner_folders(const std::stop_token& stop,
                                                                  nlohmann::json& roots_json) const;
  [[nodiscard]] Result<nlohmann::json> analyze_duplicates(const std::stop_token& stop);
  // space.duplicates with mode "photos": the near-duplicate photos of the same folders, stored as
  // candidates of "space.photo_duplicates".
  [[nodiscard]] Result<nlohmann::json> analyze_photo_duplicates(const std::stop_token& stop);
  // space.owner_decisions: the cold folders of the owner's profile and the installed programs
  // in uninstall areas, stored as owner-decision candidates.
  [[nodiscard]] Result<nlohmann::json> analyze_decisions(const std::stop_token& stop);
  // space.indexed_folders: the build-output and cache folders Windows Search indexes, stored
  // as indexed-folder candidates.
  [[nodiscard]] Result<nlohmann::json> analyze_indexed_folders(const std::stop_token& stop);
  // The cold folders of `tree` (the owner's profile, `profile`) and the installed programs to
  // review; what could not be judged goes to `notes`.
  [[nodiscard]] std::vector<scanall::Finding> cold_folders(const space::ScanResult& tree,
                                                           const std::filesystem::path& profile,
                                                           WallTime now,
                                                           nlohmann::json& notes) const;
  [[nodiscard]] std::vector<scanall::ProgramFinding> review_programs(
      nlohmann::json& notes, const std::stop_token& stop) const;
  [[nodiscard]] nlohmann::json request_scan();
  // Queues an early purge of quarantine run `run` for the cleaning worker.
  [[nodiscard]] Result<nlohmann::json> request_purge(PlanId run);
  // Purges the runs the owner asked to purge early (the cleaning worker only).
  void run_purges();
  [[nodiscard]] Result<nlohmann::json> space_top(const nlohmann::json& params);
  // `work` on the executor, or the reason cleaning is unavailable.
  [[nodiscard]] Result<nlohmann::json> on_executor(
      const std::function<Result<nlohmann::json>(Executor& executor)>& work) const;

  ServiceSources sources_;
  ServiceSettings settings_;
  std::function<void()> wake_;
  actions::PolicyEngine engine_;
  actions::StorePlanSource plans_;
  TrayTasks tasks_;
  sched::StoreScheduleRepository repository_;
  sched::Scheduler scheduler_;
  packs::Registry registry_;  // the cleaning worker only

  // Built once, by the cleaning worker, when the owner context is first available.
  std::unique_ptr<OwnerContext> owner_;
  std::optional<safety::SafeRemove> remove_;
  std::unique_ptr<actions::Quarantine> quarantine_;
  std::unique_ptr<Executor> executor_;
  std::atomic<Executor*> ready_{nullptr};
  std::optional<MonoTime> owner_tried_;  // the cleaning worker only
  bool auto_pending_ = false;            // the cleaning worker only

  mutable std::mutex mutex_;  // guards the members below
  std::optional<std::string> owner_problem_;
  std::vector<std::string> problems_;
  std::vector<packs::RelocationRecipe> recipes_;    // of registry_
  std::vector<map::Area> uninstall_areas_;          // of registry_: reclaim kind uninstall
  std::vector<map::Area> index_areas_;              // of registry_: indexer::kept_out categories
  std::vector<rules::CleaningRule> project_rules_;  // of registry_: clean cases of projects
  std::vector<ipc::PackStatus> pack_status_;
  std::vector<PlanId> purges_;  // runs to purge early, oldest request first; the first runs now
  std::vector<std::string> startup_items_;  // startup::Item::id of each id of startup.list, 1..n
  bool scan_requested_ = false;
  bool scanning_ = false;
  double progress_ = 0.0;
  std::optional<WallTime> last_completed_;
  std::map<std::string, SpaceEntry, std::less<>> space_;
  std::deque<std::string> space_queue_;
};

}  // namespace myclean::cleaning
