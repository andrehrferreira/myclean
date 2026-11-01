#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/model/units.hpp"

// Pressure detection (governor spec, "Pressure Detection" and "Pressure Clear Hysteresis"): one
// sample of the system's signals per second in, the pressure state out.
namespace myclean::governor {

// The system's signals at one second; also the "pressure metrics" every governor event records.
// A rate (CPU, hard faults) covers the time since the previous sample.
struct PressureSample {
  MonoTime time;
  double cpu_percent = 0.0;  // total CPU, share of all logical processors
  Bytes available;
  Bytes commit;
  Bytes commit_limit;
  double hard_faults_per_second = 0.0;
};

enum class PressureState { normal, pressure };

[[nodiscard]] std::string_view to_string(PressureState state);
[[nodiscard]] std::optional<PressureState> parse_pressure_state(std::string_view text);

struct PressureStatus {
  PressureState state = PressureState::normal;
  // In the pressure state: the signals past their entry threshold (dwell included) at the
  // latest sample that had any, in Reason order. Empty in the normal state.
  std::vector<Reason> reasons;
  // In the pressure state: every signal is at its exit threshold and the clear timer runs. The
  // ladder takes no new step meanwhile.
  bool clearing = false;
};

// Entry: CPU above its threshold for the dwell, available memory below its threshold, commit
// above its share of the commit limit, or hard faults above their threshold for the dwell. In
// game mode both dwells are PressurePolicy::game_dwell. Exit: every signal at its exit threshold
// (CPU and commit at most, available memory at least, hard faults at most) for clear_after
// without a break; any signal past its exit threshold restarts that timer.
//
// Durations are measured on MonoTime: a run of samples counts from the time of the sample
// before its first one (the time its rates cover), so with one sample per second the 15th
// consecutive sample above 90 % completes a 15 s dwell. The very first sample covers 1 s.
class PressureDetector {
 public:
  explicit PressureDetector(PressurePolicy policy) : policy_(policy) {}

  // A reloaded policy; the timers in progress keep running.
  void set_policy(const PressurePolicy& policy) { policy_ = policy; }

  const PressureStatus& update(const PressureSample& sample, bool game_mode);

  [[nodiscard]] const PressureStatus& status() const { return status_; }

 private:
  PressurePolicy policy_;
  std::optional<MonoTime> previous_;
  std::optional<MonoTime> cpu_since_;
  std::optional<MonoTime> faults_since_;
  std::optional<MonoTime> calm_since_;
  PressureStatus status_;
};

}  // namespace myclean::governor
