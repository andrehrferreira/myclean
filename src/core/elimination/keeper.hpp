#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/elimination/pauses.hpp"
#include "core/elimination/suspects.hpp"
#include "core/health/analysis.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/os/process_control.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/process_collector.hpp"

namespace myclean::elimination {

// The alert that reminds the owner to start a suspect again after a guided pause
// (data/alerts.toml rule freeze.guided-pause-ended); the entity is the plan id.
inline constexpr std::string_view guided_end_metric = "freeze.guided_pause_ended";

// The pauses as the PauseSuspect action sees them. Implemented by PauseKeeper.
class IPauses {
 public:
  virtual ~IPauses() = default;
  IPauses(const IPauses&) = delete;
  IPauses& operator=(const IPauses&) = delete;
  IPauses(IPauses&&) = delete;
  IPauses& operator=(IPauses&&) = delete;

  // Starts the pause of plan `plan`: `image` in `mode` for `period`. Fails with invalid_state
  // while another pause is active or before a baseline exists.
  [[nodiscard]] virtual Result<PauseRecord> pause(PlanId plan, std::string_view image,
                                                  PauseMode mode, std::chrono::hours period) = 0;
  // Ends the active pause of plan `plan` now (a suspend pause resumes what it suspended); does
  // nothing when it has ended already. Fails with not_found when the plan has no pause.
  [[nodiscard]] virtual Result<void> end(PlanId plan, EndReason reason) = 0;
  // The pause of plan `plan`; nullopt when it has none.
  [[nodiscard]] virtual Result<std::optional<PauseRecord>> record(PlanId plan) const = 0;
  // The pause not restored yet; nullopt when none.
  [[nodiscard]] virtual Result<std::optional<PauseRecord>> active() const = 0;

 protected:
  IPauses() = default;
};

struct KeeperSources {
  os::IProcessControl* processes = nullptr;  // required for a suspend pause
  // The telemetry component's newest process table; null before the first sample.
  std::function<std::shared_ptr<const telemetry::ProcessView>()> view;
  // Whether a process must never be suspended: system, game and allowed classes, MyClean, the
  // owner's foreground app (the governor). Empty: every process is protected.
  std::function<bool(const os::ProcessKey&)> protects;
  health::IAlertSink* alerts = nullptr;  // the reminder after a guided pause; null: none
};

// Keeps the pauses of guided elimination (input-watchdog spec, "One Suspect at a Time",
// "Automatic Restore", "Incident Rate Comparison"). Thread-safe: one lock.
//  - At most one pause is active.
//  - A suspend pause suspends every process of the suspect in the owner's session (session other
//    than 0) that `protects` leaves alone; each is recorded in suspect_pause_process before it is
//    suspended, resumed exactly as often as recorded when the pause ends, and forgotten once
//    resumed or gone. Nothing that is not recorded is resumed.
//  - A guided pause touches no process: tick() records the spans in which no process of the
//    suspect runs, and only those count.
//  - tick() ends a pause whose period is over; recover() (the service's start) ends every suspend
//    pause a crash or a restart left behind at once and the guided ones whose period is over;
//    stop() (the clean stop) ends every suspend pause. A guided pause survives restarts.
//  - Ending a pause stores the incident rate during its spans, writes a "restore" entry with the
//    plan to action_log, and after a guided pause reminds the owner to start the suspect again.
// `store` (opened with elimination_migrations(), input and action migrations) and `clock` must
// outlive the keeper.
class PauseKeeper final : public IPauses {
 public:
  // A guided pause's open span is written at least this often.
  static constexpr std::chrono::seconds span_flush{60};

  PauseKeeper(store::Store& store, const os::IClock& clock, KeeperSources sources)
      : store_(&store), clock_(&clock), sources_(std::move(sources)) {}

  [[nodiscard]] Result<PauseRecord> pause(PlanId plan, std::string_view image, PauseMode mode,
                                          std::chrono::hours period) override;
  [[nodiscard]] Result<void> end(PlanId plan, EndReason reason) override;
  [[nodiscard]] Result<std::optional<PauseRecord>> record(PlanId plan) const override;
  [[nodiscard]] Result<std::optional<PauseRecord>> active() const override;

  // Every few seconds: the spans of a guided pause, the end of a pause whose period is over (or
  // whose resume failed before).
  void tick();
  void recover();
  void stop();

  // The latest error of tick(), recover() or stop(); empty when the last one worked.
  [[nodiscard]] std::string problem() const;

 private:
  // The processes of `image` that may be suspended now.
  [[nodiscard]] std::vector<os::ProcessKey> suspendable(std::string_view image) const;
  // Whether a process of `image` runs now; nullopt without a process table.
  [[nodiscard]] std::optional<bool> running(std::string_view image) const;
  // Stores pause `record` (its id and baseline filled in) and its processes, and logs it; fails
  // while another pause is active or before a baseline exists.
  [[nodiscard]] Result<void> store_pause(PauseRecord& record,
                                         std::span<const os::ProcessKey> processes,
                                         std::span<const WallTime> times);
  // Resumes and forgets what suspend pause `pause` recorded; the first failure, if any (what
  // failed stays recorded).
  [[nodiscard]] std::optional<Error> resume_recorded(const PauseRecord& pause);
  // Resumes `paused` as often as it was suspended; an error when a resume failed.
  [[nodiscard]] std::optional<Error> resume(const PausedProcess& paused) const;
  // Stores the end of `pause`: its spans, the rate during them, the restore log entry.
  [[nodiscard]] Result<void> close(const PauseRecord& pause, EndReason reason);
  [[nodiscard]] Result<void> finish(const PauseRecord& pause, EndReason reason);
  // Runs `step` on the active pause, if any, and keeps its error as the problem.
  void settle(const std::function<Result<void>(const PauseRecord&)>& step);
  // Records the guided pause's span up to now: opens one while no process of the suspect runs,
  // closes it when one runs again, and writes it every span_flush (at once with `flush`).
  [[nodiscard]] Result<void> track(const PauseRecord& pause, bool flush = false);

  store::Store* store_;
  const os::IClock* clock_;
  KeeperSources sources_;
  mutable std::mutex mutex_;            // guards the members below and every pause change
  std::optional<WallTime> open_span_;   // the guided pause's span being recorded
  std::optional<MonoTime> span_saved_;  // when it was last written
  std::string problem_;
};

}  // namespace myclean::elimination
