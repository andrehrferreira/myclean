#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/events.hpp"
#include "core/games/detector.hpp"
#include "core/games/fullscreen.hpp"
#include "core/governing/executor.hpp"
#include "core/governing/job_book.hpp"
#include "core/governing/records.hpp"
#include "core/governor/classifier.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/game_mode.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/ladder.hpp"
#include "core/governor/orphans.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/pressure.hpp"
#include "core/model/actor.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/rules/schema.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/perf_counters.hpp"
#include "core/telemetry/process_collector.hpp"

// The resource governor in the service (governor spec): classifies every starting process and
// puts build and test trees into their jobs, evaluates pressure once per second from the
// telemetry pipeline's samples, runs the escalation ladder, detects games and keeps game mode,
// finds orphaned polling loops and ends those of the patterns the owner allowed,
// tracks job membership, recovers what a crashed run left and fails open on a clean stop. The
// decisions are core/governor's; this module applies them (or, in observe mode, only records
// them) and keeps the records of core/governing/records.hpp.
namespace myclean::governing {

// What the owner's tray last reported (governor.user_state).
struct UserState {
  games::NotificationState state = games::NotificationState::accepts_notifications;
  std::uint32_t foreground_pid = 0;  // 0: no window has the foreground
};

// What game detection is built from, as read for the owner, and what could not be read.
struct GameScan {
  games::GameSources sources;
  std::vector<std::string> problems;
};

// The machine and the data the governor works with: Win32 adapters and the service's telemetry
// in the service, fakes in tests. Pointers must outlive the governor; an empty function or null
// pointer means the source is absent.
struct GovernorSources {
  os::IJobControl* jobs = nullptr;           // required
  os::IProcessControl* processes = nullptr;  // required
  // The telemetry pipeline's newest process table; null before the first sample.
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // The newest system-wide value of a metric of core/telemetry/metrics.hpp.
  std::function<std::optional<double>(std::string_view metric)> metric;
  // The process classes of the loaded knowledge packs. An error keeps the previous classes.
  std::function<Result<rules::ClassSet>()> classes;
  // data/governor.toml. An error keeps the previous policy (the defaults at first).
  std::function<Result<governor::GovernorPolicy>()> policy;
  // The owner's launcher roots and GameConfigStore; absent: only the owner's list and the
  // fullscreen signal can make a game.
  std::function<GameScan()> games;
  // The full image path of a running process the governor did not see start.
  std::function<std::optional<std::filesystem::path>(std::uint32_t pid, WallTime created)>
      image_path;
  // The command line of a running process whose start the governor did not see with one; read
  // once per process, only for the roots of possible orphaned loops.
  std::function<std::optional<std::string>(std::uint32_t pid, WallTime created)> command_line;
  // GPU engine counters for the fullscreen signal; opened only while there is a candidate.
  telemetry::IPerfCounters* gpu = nullptr;
};

// Thread-safe: every public method takes one lock. tick() and the start events come from the
// service's governor thread, the owner's requests from IPC threads.
class Governor {
 public:
  static constexpr std::chrono::minutes reload_interval{5};
  // A process missing from the process table this long after it was created has exited.
  static constexpr std::chrono::seconds table_grace{10};
  // How often the process table is searched for orphaned polling loops.
  static constexpr std::chrono::seconds orphan_interval{10};
  // With no new process table or no CPU or commit metric this long, the ladder's steps 1-3 are
  // rolled back (reason telemetry_stalled): the governor acts on what it sees, so it lets go when
  // it is blind. Suspends wait for the owner, as when pressure clears.
  static constexpr std::chrono::seconds stall_limit{30};
  // A resume the OS refused after the owner's choice or a pause is tried again this often, this
  // many times; the processes stay suspended for the tray's Resume in between and after.
  static constexpr std::chrono::seconds resume_retry_interval{10};
  static constexpr int resume_retries = 3;

  // `store` and `clock` must outlive the governor.
  Governor(GovernorSources sources, store::Store& store, const os::IClock& clock);
  Governor(const Governor&) = delete;
  Governor& operator=(const Governor&) = delete;
  Governor(Governor&&) = delete;
  Governor& operator=(Governor&&) = delete;
  ~Governor() = default;

  // Loads the policy, the classes, the owner's allow list and game sources, then reconciles what
  // an earlier run left (crash recovery): its jobs are re-opened (owner checked) and adopted with
  // the current policy or released, every persisted suspend is resumed exactly as recorded, and a
  // persisted pause that has not ended goes on. Fails only when the store cannot be read.
  [[nodiscard]] Result<void> start();

  // A process started: classification, and for a build or test tree root its jobs.
  void on_start(const events::ProcessStart& start);
  // A process exited: its fullscreen verdict is dropped (its PID will be reused).
  void on_stop(const events::ProcessStop& stop);
  // The 1 s work: reload when due, jobs, game detection and game mode, pressure and the ladder.
  void tick();
  // Clean stop (fail-open): every ladder step rolled back and every suspend resumed, every job
  // lifted and released, every persisted suspend resumed as recorded. Idempotent.
  void stop();

  // The methods of core/ipc/governor.hpp.
  [[nodiscard]] nlohmann::json status() const;
  // How many commands the owner's choice caused.
  [[nodiscard]] std::size_t choose(const governor::ProcessKey& root, governor::OwnerChoice choice);
  // 0 minutes ends a pause. The time the pause ends, if paused. The pause is persisted, so a
  // restarted service keeps it until that time.
  [[nodiscard]] std::optional<WallTime> pause(std::chrono::minutes minutes);
  [[nodiscard]] governor::GameMode set_override(governor::GameModeOverride value);
  void set_user_state(const UserState& state);
  // The owner's answer to the orphaned-loop proposal `id` (end_now, always, ignore), or forgetting
  // the stored rule `id` (forget); recorded with actor `owner`. How many trees were ended (in
  // observe mode: recorded as ended). Fails with not_found for an unknown id, a proposal whose
  // loops ended or are no longer orphaned included.
  [[nodiscard]] Result<std::size_t> decide_orphans(std::int64_t id,
                                                   governor::OrphanDecision decision);

  // What is not working, for the component's health.
  [[nodiscard]] std::vector<std::string> problems() const;

  // Whether `key` must never be suspended by anyone (guided freeze elimination): a process of a
  // protected class (system, game, allowed; every service), MyClean itself, the owner's foreground
  // process, and, failing safe, any process while the governor is not running, one it does not
  // know or while the tray has not reported the foreground recently.
  [[nodiscard]] bool protects(const governor::ProcessKey& key) const;

 private:
  // What the governor knows about a running process.
  struct Known {
    governor::ProcessFacts facts;
    rules::ProcessClass process_class = rules::ProcessClass::normal;  // classify()
    std::optional<games::GameVerdict> game;                           // from the image alone
    std::optional<governor::ProcessKey> parent;
    bool command_line_read = false;        // GovernorSources::command_line was asked
    std::optional<std::uint32_t> session;  // from the process table; 0: a service
  };
  struct Notification {
    std::int64_t id = 0;
    WallTime time;
    governor::Command command;
  };
  // A resume the OS refused, tried again from tick().
  struct ResumeRetry {
    int attempts = 0;
    MonoTime next;
  };
  // "End N loops of <pattern>": a pattern without a rule with at least min_trees live trees.
  struct OrphanProposal {
    std::int64_t id = 0;  // kept while the pattern stays proposed
    WallTime time;        // when it was first proposed
    std::string pattern;
    std::string image;
    std::vector<std::vector<governor::ProcessKey>> trees;  // members, the root first
  };

  void reload();
  void apply_policy(const governor::GovernorPolicy& policy);
  void scan_games();
  void recover();
  // The saved priorities of an earlier run; `adopted_members`: the PIDs of jobs adopted again.
  void recover_states(const std::set<std::uint32_t>& adopted_members);
  void fail_open(governor::Reason reason, std::string_view detail);
  void run(const governor::Command& command);
  // Runs the commands of an owner's choice, a pause or a retry. A resume the OS refused leaves its
  // processes suspended: the ladder keeps them suspended, so their notification keeps Resume, and
  // the owner is told (problems). True when one was refused.
  [[nodiscard]] bool run_owner(const std::vector<governor::Command>& commands);
  void retry_resumes();
  // tick() without a snapshot for the ladder: after stall_limit, steps 1-3 are rolled back.
  void check_stall(const telemetry::ProcessView* view);
  void restore_pause();
  void store_pause();
  [[nodiscard]] Known learn(governor::ProcessFacts facts,
                            std::optional<governor::ProcessKey> parent) const;
  void update_known(const telemetry::ProcessView& view);
  void place(const governor::ProcessKey& key, const Known& known, rules::ProcessClass process_class,
             governor::JobPlacement placement);
  // The descendants of `root` in the process view, which it started before it was placed and so
  // did not inherit its job: without protected processes (and what lies below them) and without
  // processes already in a MyClean tree job.
  [[nodiscard]] std::vector<governor::ProcessKey> earlier_descendants(
      const governor::ProcessKey& root) const;
  // A starting process whose parent is in a MyClean tree job but which is not in it (it started
  // before the root was placed) is assigned to that job, unless it is protected.
  void join_parent_job(const governor::ProcessKey& key, const Known& known,
                       const governor::ProcessKey& parent);
  // Protected from placement: a protected class, a MyClean process or the foreground process.
  [[nodiscard]] bool spared(const governor::ProcessKey& key, std::string_view image) const;
  [[nodiscard]] std::optional<governor::PressureSample> read_sample() const;
  void govern(const telemetry::ProcessView& view, const governor::PressureSample& sample);
  void observe_fullscreen();
  void update_game_mode();
  void scan_orphans(const telemetry::ProcessView& view);
  [[nodiscard]] std::vector<governor::OrphanProcess> orphan_table(
      const telemetry::ProcessView& view) const;
  void read_command_lines(std::vector<governor::OrphanProcess>& table,
                          const std::vector<std::size_t>& roots);
  void propose(const governor::OrphanGroup& group,
               std::vector<std::vector<governor::ProcessKey>> trees,
               std::vector<OrphanProposal>& proposals);
  void end_always(const governor::OrphanGroup& group,
                  const std::vector<std::vector<governor::ProcessKey>>& trees);
  void end_orphan_tree(const std::vector<governor::ProcessKey>& members, const Actor& actor);
  [[nodiscard]] Result<void> store_orphan_rule(const OrphanProposal& proposal,
                                               governor::OrphanRule rule);
  [[nodiscard]] nlohmann::json orphans_json() const;
  [[nodiscard]] std::size_t apply_class_limits();
  [[nodiscard]] rules::ProcessClass class_of(const governor::ProcessKey& key) const;
  [[nodiscard]] bool allowed(std::string_view image) const;
  [[nodiscard]] bool under_class(const governor::ProcessKey& key,
                                 rules::ProcessClass process_class) const;
  [[nodiscard]] bool under_observed_root(const governor::ProcessKey& key) const;
  [[nodiscard]] std::optional<UserState> fresh_user_state() const;
  [[nodiscard]] nlohmann::json games_json() const;
  [[nodiscard]] nlohmann::json jobs_json() const;
  [[nodiscard]] nlohmann::json trees_json() const;
  void record(EventKind kind, const std::optional<governor::ProcessKey>& root,
              std::string_view image, std::optional<rules::ProcessClass> process_class,
              governor::Reason reason, std::string detail, bool audit);

  GovernorSources sources_;
  store::Store* store_;
  const os::IClock* clock_;
  Recorder recorder_;
  JobBook jobs_;
  Executor executor_;
  governor::GovernorPolicy policy_;
  governor::Classifier classifier_{rules::ClassSet{}};
  governor::PressureDetector pressure_;
  governor::LadderEngine ladder_;
  games::GameDetector detector_;
  std::optional<games::Gpu3dSampler> gpu_;
  governor::GameModeOverride override_ = governor::GameModeOverride::automatic;
  governor::GameMode game_mode_;
  std::vector<std::string> allowed_;  // the owner's Always-allow images
  std::map<governor::ProcessKey, Known> known_;
  std::set<governor::ProcessKey> observed_roots_;  // observe mode: the roots that would have jobs
  std::optional<UserState> user_state_;
  std::optional<MonoTime> user_state_at_;
  bool rescan_games_ = false;
  std::optional<WallTime> last_view_;
  std::optional<governor::PressureSample> sample_;
  std::optional<MonoTime> reloaded_at_;
  std::optional<WallTime> paused_until_;
  std::optional<MonoTime> governed_at_;  // the ladder's latest snapshot (or the start)
  bool stalled_ = false;                 // steps rolled back for this stall
  std::deque<Notification> notifications_;
  std::map<governor::ProcessKey, ResumeRetry> retries_;  // by the root the owner chose for
  std::int64_t next_notification_ = 1;
  std::vector<OrphanRuleRecord> orphan_rules_;
  std::vector<OrphanProposal> orphan_proposals_;
  std::int64_t next_orphan_proposal_ = 1;
  std::optional<MonoTime> orphans_at_;
  std::set<governor::ProcessKey> orphans_ended_;  // roots ended (or recorded so) and still listed
  std::map<std::string, std::string, std::less<>> problems_;  // by source
  bool started_ = false;
  bool stopped_ = false;
  mutable std::mutex mutex_;
};

}  // namespace myclean::governing
