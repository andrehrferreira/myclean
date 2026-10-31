#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
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
using Request = std::variant<SnoozeAlert, DismissAlert, ExplainProcess, SelectRange>;

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
};

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
// Not thread-safe: one poll thread owns it.
class Poller {
 public:
  static constexpr int top_trees = 25;
  static constexpr std::chrono::seconds slow_interval{60};
  // Points per chart of a past range: about one per horizontal pixel of a chart.
  static constexpr int range_points = 600;
  // How far back the Alerts section lists alerts.
  static constexpr std::chrono::days recent_alerts{30};

  // `window` is how far back the live charts reach: nothing older is fetched. `incidents`, when
  // set, is forwarded at every poll and must outlive the poller.
  Poller(IServiceLink& link, std::chrono::seconds window, IncidentQueue* incidents = nullptr)
      : link_(&link), window_(window), incidents_(incidents) {}

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
  [[nodiscard]] RangeHistory fetch_range(WallTime now);

  IServiceLink* link_;
  std::chrono::seconds window_;
  IncidentQueue* incidents_;
  bool connected_ = false;  // the previous poll reached the service
  bool dashboard_open_ = false;
  std::optional<WallTime> next_slow_;
  std::optional<WallTime> next_range_;
  std::optional<AlertSummary> badge_;
  Range range_ = Range::live;
  std::map<SeriesKey, WallTime> newest_;  // per metrics.latest series
};

// Whether the dashboard charts `key` over past ranges: the system CPU and memory lines, each
// adapter's GPU utilization and every disk.* series.
[[nodiscard]] bool charted(const SeriesKey& key);

}  // namespace myclean::tray_model
