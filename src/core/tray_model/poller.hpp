#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/cleaning.hpp"
#include "core/tray_model/governor.hpp"
#include "core/tray_model/incident_queue.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/service_link.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

// The span of the dashboard's charts: the live window, or a past range served by metrics.query.
enum class Range { live, hour, day, month };

// How far back `range` reaches; `live` is the live window.
[[nodiscard]] std::chrono::seconds range_length(Range range, std::chrono::seconds live);

// What the dashboard asks of the service; the poll thread sends it with its next poll.
struct SnoozeAlert {
  std::string rule;
  std::string entity;
  std::chrono::minutes duration{};
};
struct DismissAlert {
  std::string rule;
  std::string entity;
};
struct ExplainProcess {
  std::uint32_t pid = 0;
};
struct SelectRange {
  Range range = Range::live;
};
// The approval inbox: a cleaning scan, a plan's details shown or hidden, the owner's decision,
// an undo, a restore from quarantine; a Space list, a cleanup proposed from it, and the one-click
// action of an owner decision.
struct ScanNow {};
struct OpenPlan {
  std::int64_t id = 0;
};
struct ClosePlan {
  std::int64_t id = 0;
};
struct ApprovePlan {
  std::int64_t id = 0;
  Selection selection;
};
struct RejectPlan {
  std::int64_t id = 0;
};
struct UndoPlan {
  std::int64_t id = 0;
};
struct RestoreQuarantine {
  std::int64_t plan = 0;
  std::optional<std::int64_t> item;  // nullopt: every item of the plan
};
struct PurgeQuarantine {
  std::int64_t plan = 0;  // the run whose held items are deleted now
};
struct LoadSpace {
  SpaceList list = SpaceList::top;
  std::string volume;    // space.top: "C:"
  bool refresh = false;  // start a new analysis
};
struct ProposeCleanup {
  std::vector<std::int64_t> candidates;  // Space candidate ids
};
// The owner's one click on an owner decision: moving a cold folder to the HDD archive, or
// uninstalling a program (plans.owner_action creates the plan already approved).
struct OwnerAction {
  std::int64_t candidate = 0;  // an owner-decision candidate id
};
// The governor: the owner's answer to the notification of a tree (resume, kill, allow_once,
// always_allow), a pause (0 minutes ends one) and the game-mode override (auto, on, off).
struct ChooseForTree {
  GovernorProcess root;
  std::string choice;
};
struct PauseGovernor {
  std::chrono::minutes minutes{};
};
struct SetGameMode {
  std::string value;
};
// The owner's answer to an orphaned-loop proposal (end_now, always, ignore), or forgetting a
// stored rule (forget, `id` the rule's).
struct DecideOrphans {
  std::int64_t id = 0;
  std::string decision;
};
using Request = std::variant<SnoozeAlert, DismissAlert, ExplainProcess, SelectRange, ScanNow,
                             OpenPlan, ClosePlan, ApprovePlan, RejectPlan, UndoPlan,
                             RestoreQuarantine, PurgeQuarantine, LoadSpace, ProposeCleanup,
                             OwnerAction, ChooseForTree, PauseGovernor, SetGameMode, DecideOrphans>;

// The tray's executor of user-scope plans, as the poller drives it: the poller takes a task
// (user_tasks.next) only from a reachable service and only while the runner is idle, and
// delivers the runner's report (user_tasks.report) before it takes the next one.
class ITaskRunner {
 public:
  struct Report {
    std::int64_t plan = 0;
    nlohmann::json result;  // the `result` record of user_tasks.report
    std::string summary;    // for the dashboard: "Plan 12 verified"
  };

  virtual ~ITaskRunner() = default;
  ITaskRunner(const ITaskRunner&) = delete;
  ITaskRunner& operator=(const ITaskRunner&) = delete;
  ITaskRunner(ITaskRunner&&) = delete;
  ITaskRunner& operator=(ITaskRunner&&) = delete;

  // No task runs and no report waits for delivery.
  [[nodiscard]] virtual bool idle() const = 0;
  // Starts `task`, the "task" object of a user_tasks.next answer, on the runner's own thread.
  virtual void start(nlohmann::json task) = 0;
  // The finished task's report, until delivered() is called.
  [[nodiscard]] virtual std::optional<Report> report() const = 0;
  virtual void delivered() = 0;
  // What runs now, for the dashboard; empty when nothing does.
  [[nodiscard]] virtual std::string activity() const = 0;

 protected:
  ITaskRunner() = default;
};

// The charts of a past range, fetched at `time`.
struct RangeHistory {
  Range range = Range::live;
  WallTime time;
  std::vector<Series> series;
};

// Everything one poll learned. A member that is nullopt was not asked for, or its method failed
// (a service that does not offer it yet); the dashboard shows "n/a" for it.
struct PollResult {
  WallTime time;
  bool reachable = false;
  std::string error;  // why the service is unreachable, or empty
  std::optional<LatestReply> latest;
  // Samples metrics.latest does not carry: the ones between two polls that were missed.
  std::vector<Series> history;
  std::optional<std::vector<ProcessTreeRow>> top_by_cpu;
  std::optional<std::vector<ProcessTreeRow>> top_by_memory;
  std::optional<std::vector<DriveHealthRow>> drives;
  std::optional<AlertSummary> badge;
  std::optional<std::vector<AlertRow>> alerts;
  std::optional<std::vector<FreezeGroupRow>> freezes;
  std::optional<RangeHistory> range;
  std::optional<Result<Explanation>> explanation;
  // Why a snooze or dismiss failed; empty when it worked.
  std::optional<std::string> alert_action;
  std::optional<IncidentQueue::Counts> incidents;
  CleanupPoll cleanup;
  std::optional<GovernorStatus> governor;
  // Why the owner's last governor request failed; empty when it worked.
  std::optional<std::string> governor_action;
};

// What the tray reads of the owner's session for governor.user_state; nullopt when it cannot be
// read this second.
using UserStateSource = std::function<std::optional<UserStateReading>()>;

// Polls the service, once per second, for what the tray shows.
//
// Every poll: metrics.latest (tooltip, live charts; they keep filling while the dashboard is
// closed so that it opens with recent history) and alerts.badge (icon color and badge), and
// forwards the watchdog's incidents. Every slow_interval, and at once after connecting:
// disks.health (tooltip) and alerts.list (toasts), which is also fetched when the badge changed.
// While the dashboard is open also: the top process trees by CPU and by memory, any 1 s sample
// missed between two polls (so the charts drop no sample), input.summary at the slow interval
// and after incidents were sent, and the past range the charts show, at the slow interval.
// Opening the dashboard fetches everything at once and starts on the live range.
//
// The approval inbox: plans.list every plans_interval (the badge counts the pending plans), at
// once after connecting, after any request on a plan and when a cleaning scan ends; with it,
// while the dashboard is open, quarantine.list and the details of the plans the owner opened.
// cleaning.status at every poll while the dashboard is open or a scan runs, else with the plans.
// Space lists when the owner asks, and again at every poll while their analysis runs. User tasks:
// every poll, when a task runner is set, the finished task's report is delivered and, while the
// runner is idle, the next task is taken. The governor: every poll, governor.user_state with the
// owner's session state when a source is set, then governor.status (its notifications become
// toasts); the owner's choices, pause and game-mode override are sent before it. Not thread-safe:
// one poll thread owns it.
class Poller {
 public:
  static constexpr int top_trees = 25;
  static constexpr std::chrono::seconds slow_interval{60};
  // Points per chart of a past range: about one per horizontal pixel of a chart.
  static constexpr int range_points = 600;
  // How far back the Alerts section lists alerts.
  static constexpr std::chrono::days recent_alerts{30};
  static constexpr std::chrono::seconds plans_interval{10};

  // `window` is how far back the live charts reach: nothing older is fetched. `incidents`, when
  // set, is forwarded at every poll; `tasks`, when set, runs the user tasks. Both must outlive
  // the poller. `user_state`, when set, is read and sent at every poll.
  Poller(IServiceLink& link, std::chrono::seconds window, IncidentQueue* incidents = nullptr,
         ITaskRunner* tasks = nullptr, UserStateSource user_state = {})
      : link_(&link),
        window_(window),
        incidents_(incidents),
        tasks_(tasks),
        user_state_(std::move(user_state)) {}

  [[nodiscard]] PollResult poll(WallTime now, bool dashboard_open,
                                const std::vector<Request>& requests = {});

 private:
  [[nodiscard]] std::optional<Series> query(const SeriesKey& key, std::int64_t from,
                                            std::int64_t to, std::optional<int> max_points);
  // Takes in the dashboard's state and the range the owner chose.
  void select(bool dashboard_open, const std::vector<Request>& requests);
  // Everything after metrics.latest and the requests, once the service answered.
  void fetch_rest(WallTime now, bool dashboard_open, PollResult& result);
  void fill_gaps(const LatestReply& latest, bool dashboard_open, PollResult& result);
  void handle(const Request& request, PollResult& result);
  void load_space(const LoadSpace& load, PollResult& result);
  // Asks again for the Space lists whose analysis ran at the previous poll, unless a request
  // asks for one anyway.
  void ask_again(bool dashboard_open, const std::vector<Request>& requests, PollResult& result);
  // The inbox requests; true when the plans must be fetched again.
  bool handle_cleanup(const Request& request, PollResult& result);
  void fetch_cleanup(WallTime now, bool dashboard_open, bool refresh, PollResult& result);
  void run_tasks(PollResult& result);
  // Sends the governor's requests; other requests are left alone.
  void handle_governor(const Request& request, PollResult& result);
  void fetch_governor(PollResult& result);
  [[nodiscard]] RangeHistory fetch_range(WallTime now);

  IServiceLink* link_;
  std::chrono::seconds window_;
  IncidentQueue* incidents_;
  ITaskRunner* tasks_;
  UserStateSource user_state_;
  bool connected_ = false;  // the previous poll reached the service
  bool dashboard_open_ = false;
  std::optional<WallTime> next_slow_;
  std::optional<WallTime> next_range_;
  std::optional<AlertSummary> badge_;
  Range range_ = Range::live;
  std::map<SeriesKey, WallTime> newest_;  // per metrics.latest series
  std::optional<WallTime> next_plans_;
  std::optional<CleaningStatus> status_;
  std::set<std::int64_t> open_plans_;  // whose details the dashboard shows
  std::vector<LoadSpace> analysing_;   // Space lists whose analysis runs
};

// Whether the dashboard charts `key` over past ranges: the system CPU and memory lines, each
// adapter's GPU utilization and every disk.* series.
[[nodiscard]] bool charted(const SeriesKey& key);

}  // namespace myclean::tray_model
