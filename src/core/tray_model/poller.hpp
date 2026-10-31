#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/tray_model/replies.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::tray_model {

// The tray's connection to the service. Implemented over the pipe in src/tray; tests use a fake.
class IServiceLink {
 public:
  virtual ~IServiceLink() = default;
  IServiceLink(const IServiceLink&) = delete;
  IServiceLink& operator=(const IServiceLink&) = delete;
  IServiceLink(IServiceLink&&) = delete;
  IServiceLink& operator=(IServiceLink&&) = delete;

  // The result of `method`, or the JSON-RPC error the service answered, or a failure to reach
  // the service at all (then reachable() is false until a later call gets through).
  [[nodiscard]] virtual Result<nlohmann::json> call(std::string_view method,
                                                    const nlohmann::json& params) = 0;
  // Whether the last call got an answer from the service, error answers included.
  [[nodiscard]] virtual bool reachable() const = 0;

 protected:
  IServiceLink() = default;
};

// Everything one poll learned. A member that is nullopt was not asked for, or its method failed
// (a service that does not offer it yet); the dashboard shows "n/a" for it.
struct PollResult {
  WallTime time;
  bool reachable = false;
  std::string error;  // why the service is unreachable, or empty
  std::optional<LatestReply> latest;
  // Samples metrics.latest does not carry: the ones between two polls that were missed, and
  // the per-processor series.
  std::vector<Series> history;
  std::optional<std::vector<ProcessTreeRow>> top_by_cpu;
  std::optional<std::vector<ProcessTreeRow>> top_by_memory;
};

// Polls the service, once per second, for what the tray shows.
//
// Always: metrics.latest, for the tooltip and the live charts, which keep filling while the
// dashboard is closed so that it opens with recent history. While the dashboard is open also:
// - the top process trees by CPU and by memory (processes.top);
// - the per-processor utilization and frequency (metrics.query; metrics.latest has only the
//   system-wide CPU values), from where the previous poll left off;
// - any 1 s sample missed between two polls (a poll that came late), from metrics.query, so
//   the charts drop no sample.
// Not thread-safe: one poll thread owns it.
class Poller {
 public:
  static constexpr int top_trees = 25;

  // `window` is how far back the live charts reach: nothing older is fetched.
  Poller(IServiceLink& link, std::chrono::seconds window) : link_(&link), window_(window) {}

  [[nodiscard]] PollResult poll(WallTime now, bool dashboard_open);

 private:
  [[nodiscard]] std::optional<Series> query(std::string_view metric, std::string_view entity,
                                            std::int64_t from, std::int64_t to);
  void fill_gaps(const LatestReply& latest, bool dashboard_open, PollResult& result);
  void poll_processors(unsigned count, WallTime now, PollResult& result);

  IServiceLink* link_;
  std::chrono::seconds window_;
  std::map<SeriesKey, WallTime> newest_;         // per metrics.latest series
  std::map<SeriesKey, std::int64_t> next_from_;  // per processor series: the next second to ask
};

}  // namespace myclean::tray_model
