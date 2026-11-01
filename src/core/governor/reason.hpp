#pragma once

#include <optional>
#include <string_view>

// Why the governor did something: the `reason` of a governor_event row (stored; fixed texts).
namespace myclean::governor {

enum class Reason {
  // Pressure signals (PressureStatus::reasons holds only these four).
  cpu,
  available_memory,
  commit,
  hard_faults,
  // Ladder bookkeeping.
  pressure_cleared,   // steps 1-3 rolled back after the clear hysteresis
  joined_tree,        // a process that joined a throttled tree got the tree's steps
  protected_process,  // "protected": a throttled process became protected (foreground, ...)
  // The owner's choices (actor `owner`).
  allow_once,
  always_allow,
  resume,
  owner_kill,
  pause,
  // Service life cycle and crash recovery.
  stop,
  orphaned_suspend,
  untrusted_job,
  policy,  // a re-adopted job's limits set to the current policy, or lifted
};

[[nodiscard]] std::string_view to_string(Reason reason);
[[nodiscard]] std::optional<Reason> parse_reason(std::string_view text);

// One of cpu, available_memory, commit and hard_faults.
[[nodiscard]] bool is_pressure_signal(Reason reason);

}  // namespace myclean::governor
