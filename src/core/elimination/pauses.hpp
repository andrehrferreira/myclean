#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/elimination/suspects.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

// The incident rates guided elimination compares (input-watchdog spec, "Incident Rate
// Comparison") and the records of the pauses (component "elimination"):
//   suspect_pause          one row per pause: its plan, suspect, mode, period, how it ended, and
//                          the incidents and hours of its baseline and of the pause
//   suspect_pause_process  (pid, creation time, count) of each process a suspend pause suspended,
//                          written before the suspend, removed once resumed (or gone)
//   suspect_pause_span     the spans a pause counts: the whole pause of a suspend pause, the
//                          spans no process of the suspect ran in for a guided one
// Creation times are stored in nanoseconds since the Unix epoch, other times in Unix seconds.
namespace myclean::elimination {

// The span a baseline covers, back from the start of a pause.
inline constexpr std::chrono::days baseline_span{7};

// Incidents over observed hours.
struct Rate {
  std::int64_t incidents = 0;
  double hours = 0.0;

  // Incidents per day; nullopt when no hour was observed.
  [[nodiscard]] std::optional<double> per_day() const;

  friend bool operator==(const Rate&, const Rate&) = default;
};

struct Span {
  WallTime from;
  WallTime to;  // excluded

  friend bool operator==(const Span&, const Span&) = default;
};

// The incidents of `times` inside `spans` (which do not overlap) and the hours they cover.
[[nodiscard]] Rate rate_in(std::span<const WallTime> times, std::span<const Span> spans);

// The baseline at `at`: the incidents of `times` in the baseline_span before `at` and its hours,
// both without the spans of earlier pauses `excluded`. nullopt while it is still being collected:
// no incident is recorded, or the first one is less than baseline_span before `at`.
[[nodiscard]] std::optional<Rate> baseline(std::span<const WallTime> times, WallTime at,
                                           std::span<const Span> excluded);

// How a pause ended. Stored as text.
enum class EndReason {
  period_ended,       // "period_ended": its period is over
  ended_by_owner,     // "ended_by_owner": the owner ended it early in the tray
  undone,             // "undone": the owner undid its plan
  service_restarted,  // "service_restarted": a suspend pause found by a restarted service
  service_stopped,    // "service_stopped": a suspend pause at the service's clean stop
  failed,             // "failed": no process could be suspended
};

[[nodiscard]] std::string_view to_string(EndReason reason);
[[nodiscard]] std::optional<EndReason> parse_end_reason(std::string_view text);

struct PauseRecord {
  std::int64_t id = 0;
  PlanId plan;
  std::string image;
  PauseMode mode = PauseMode::suspend;
  WallTime started;
  WallTime ends;  // when the period is over
  std::optional<WallTime> restored;
  std::optional<EndReason> reason;  // set with `restored`
  Rate baseline;
  std::optional<Rate> during;  // set with `restored`

  [[nodiscard]] bool active() const { return !restored.has_value(); }

  friend bool operator==(const PauseRecord&, const PauseRecord&) = default;
};

[[nodiscard]] const store::MigrationSet& elimination_migrations();

// Stores a new pause (`record.id` and `during` are ignored, `restored` must be empty) and the
// processes it is about to suspend, each counted once, in one transaction; its id.
[[nodiscard]] Result<std::int64_t> begin_pause(store::Database& db, const PauseRecord& record,
                                               std::span<const os::ProcessKey> processes);
// The processes of pause `id` still to be resumed, with how often each was suspended.
struct PausedProcess {
  os::ProcessKey process;
  int count = 0;
};
[[nodiscard]] Result<std::vector<PausedProcess>> paused_processes(store::Database& db,
                                                                  std::int64_t id);
[[nodiscard]] Result<void> forget_process(store::Database& db, std::int64_t id,
                                          const os::ProcessKey& process);
// Adds the span [from, to) to pause `id`, or extends the one that starts at `from`.
[[nodiscard]] Result<void> save_span(store::Database& db, std::int64_t id, WallTime from,
                                     WallTime to);
[[nodiscard]] Result<std::vector<Span>> spans_of(store::Database& db, std::int64_t id);
// Ends pause `id` at `restored` for `reason`, with the rate during it.
[[nodiscard]] Result<void> finish_pause(store::Database& db, std::int64_t id, WallTime restored,
                                        EndReason reason, const Rate& during);
// The pause of plan `plan`; nullopt when it has none.
[[nodiscard]] Result<std::optional<PauseRecord>> pause_of_plan(store::Database& db, PlanId plan);
// Every pause, newest first (at most `limit`).
[[nodiscard]] Result<std::vector<PauseRecord>> list_pauses(store::Database& db, std::size_t limit);
// The pause not restored yet; nullopt when none.
[[nodiscard]] Result<std::optional<PauseRecord>> active_pause(store::Database& db);
// The period of every finished pause, from its start to its restore (what a baseline leaves out).
[[nodiscard]] Result<std::vector<Span>> pause_periods(store::Database& db);

}  // namespace myclean::elimination
