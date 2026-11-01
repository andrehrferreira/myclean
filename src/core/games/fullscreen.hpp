#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/telemetry/perf_counters.hpp"

// The fullscreen + GPU signal of game detection (spec Game Detection, spike X5 §3-4): a process
// that owns the foreground while the owner's session reports a fullscreen app and that keeps the
// GPU's 3D engines busy is a game; a fullscreen video or slideshow barely uses them.
namespace myclean::games {

// SHQueryUserNotificationState, as the tray reads it in the owner's session (QUNS_* 1-7).
enum class NotificationState {
  not_present,              // QUNS_NOT_PRESENT: screen saver, locked or switching users
  busy,                     // QUNS_BUSY: a fullscreen app, also a borderless game
  running_d3d_full_screen,  // QUNS_RUNNING_D3D_FULL_SCREEN: exclusive-fullscreen Direct3D
  presentation_mode,        // QUNS_PRESENTATION_MODE
  accepts_notifications,    // QUNS_ACCEPTS_NOTIFICATIONS: ordinary desktop use
  quiet_time,               // QUNS_QUIET_TIME: first hour after setup
  app,                      // QUNS_APP: a Windows Store app is fullscreen
};

[[nodiscard]] std::string_view to_string(NotificationState state);
[[nodiscard]] std::optional<NotificationState> parse_notification_state(std::string_view text);

// True for the states in which the foreground process can be a fullscreen game: busy (seen for
// borderless fullscreen) and running_d3d_full_screen (exclusive).
[[nodiscard]] bool fullscreen_state(NotificationState state);

// The counter the 3D use is read from.
inline constexpr std::string_view gpu_engine_counter = R"(\GPU Engine(*)\Utilization Percentage)";

// Per-process 3D engine use, in percent, from the instances of gpu_engine_counter
// ("pid_<PID>_luid_0x<HIGH>_0x<LOW>_phys_<n>_eng_<n>_engtype_<TYPE>"): the engtype "3D"
// instances summed per process and adapter (LUID), each sum capped at 100, then the busiest
// adapter's sum, so the Basic Render Driver and an idle iGPU do not dilute the discrete GPU.
// Other engine types ("High Priority 3D", "Copy", "Video Decode") and malformed names are
// ignored. A process with no 3D instance is absent.
[[nodiscard]] std::map<std::uint32_t, double> gpu_3d_by_pid(
    std::span<const telemetry::CounterValue> engines);

// Samples per-process 3D use on demand. Opened only while a fullscreen candidate exists, since
// the counter has about 900 instances (spike X5 §4); the first sample after opening is empty,
// because the counter needs two readings.
class Gpu3dSampler {
 public:
  // Fails when the machine has no GPU Engine counters.
  [[nodiscard]] static Result<Gpu3dSampler> open(telemetry::IPerfCounters& counters);

  [[nodiscard]] Result<std::map<std::uint32_t, double>> sample();

 private:
  Gpu3dSampler(std::unique_ptr<telemetry::ICounterQuery> query, std::size_t index)
      : query_(std::move(query)), index_(index) {}

  std::unique_ptr<telemetry::ICounterQuery> query_;
  std::size_t index_ = 0;
};

// Uncalibrated defaults: no real game was measured yet (spike X5 §4, tasks.md 1.2). A trivial
// render loop used 8-24 %, an idle borderless window at most 0.2 %.
inline constexpr double default_min_gpu_percent = 2.0;
inline constexpr std::chrono::seconds default_gpu_window{10};
inline constexpr std::size_t default_min_gpu_samples = 5;

struct FullscreenGpuPolicy {
  double min_gpu_percent = default_min_gpu_percent;   // average 3D use that makes a game
  std::chrono::seconds window = default_gpu_window;   // how long the candidate must hold
  std::size_t min_samples = default_min_gpu_samples;  // GPU samples needed in the window
};

// One second of observation: the tray's notification state, the foreground window's process
// and its 3D use (nullopt when GPU use was not sampled this second).
struct FullscreenObservation {
  NotificationState state = NotificationState::accepts_notifications;
  std::uint32_t foreground_pid = 0;  // 0: no foreground window
  std::optional<double> gpu_3d_percent;
};

// Decides which foreground fullscreen processes are games. A process is a candidate while it
// owns the foreground in a fullscreen_state; it becomes a game once it has been the candidate
// for `window` and its 3D use averaged over the samples of the last `window` (at least
// `min_samples` of them) reaches `min_gpu_percent`. A change of foreground or state restarts
// the candidate. A game stays detected (also after it leaves the foreground) until forget().
// Not thread-safe.
class FullscreenGpuDetector {
 public:
  // `clock` must outlive the detector.
  FullscreenGpuDetector(FullscreenGpuPolicy policy, const os::IClock& clock);

  // The process whose GPU use should be sampled now: the candidate, unless it is already
  // detected. GPU use is sampled only while there is one (spec Game Detection).
  [[nodiscard]] std::optional<std::uint32_t> candidate() const;
  // Feeds one observation at the clock's monotonic time; returns the PID when this observation
  // makes it a game.
  std::optional<std::uint32_t> observe(const FullscreenObservation& observation);
  [[nodiscard]] bool detected(std::uint32_t pid) const { return detected_.contains(pid); }
  // The process exited: drops its verdict, since its PID will be reused.
  void forget(std::uint32_t pid);

 private:
  void reset();

  FullscreenGpuPolicy policy_;
  const os::IClock* clock_;
  std::optional<std::uint32_t> candidate_;
  MonoTime since_;
  std::deque<std::pair<MonoTime, double>> samples_;  // the candidate's, oldest first
  std::set<std::uint32_t> detected_;
};

}  // namespace myclean::games
