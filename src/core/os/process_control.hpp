#pragma once

#include <cstdint>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::os {

// One process, as the governor acts on it: its PID and its creation time. Every operation that
// takes a ProcessKey opens the PID and compares the creation time before it acts, so a PID that
// was reused by another process is refused (not_found) instead of being acted on.
struct ProcessKey {
  std::uint32_t pid = 0;
  WallTime created;

  friend bool operator==(const ProcessKey&, const ProcessKey&) = default;
};

// Windows priority classes (SetPriorityClass).
enum class PriorityClass { idle, below_normal, normal, above_normal, high, realtime };

// Memory priority of a process's pages (ProcessMemoryPriority): 1 (very low) to 5 (normal).
enum class MemoryPriority { very_low = 1, low = 2, medium = 3, below_normal = 4, normal = 5 };

// The EcoQoS execution-speed state of a process (ProcessPowerThrottling,
// PROCESS_POWER_THROTTLING_EXECUTION_SPEED): left to the system, forced on, or forced off.
enum class EcoQos { automatic, on, off };

// Exit codes that mean a process died because a commit failed: 0xC0000017 (STATUS_NO_MEMORY) and
// 0xC0000409 (fail-fast, where the UCRT ends a program whose allocation failure went unhandled).
// Windows reports these as a normal exit, not an abnormal one (spike X4 §3.2).
[[nodiscard]] constexpr bool is_commit_failure_exit(std::uint32_t exit_code) {
  return exit_code == 0xC0000017U || exit_code == 0xC0000409U;
}

// Per-process actions of the governor's ladder (and their rollback). Implemented by
// src/platform/jobs and by tests/fakes. Every call verifies the key first; a process that is gone
// or whose PID now belongs to another process fails with not_found. Each setter returns the
// previous state, for the rollback.
class IProcessControl {
 public:
  virtual ~IProcessControl() = default;
  IProcessControl(const IProcessControl&) = delete;
  IProcessControl& operator=(const IProcessControl&) = delete;
  IProcessControl(IProcessControl&&) = delete;
  IProcessControl& operator=(IProcessControl&&) = delete;

  [[nodiscard]] virtual Result<PriorityClass> priority(const ProcessKey& process) const = 0;
  [[nodiscard]] virtual Result<PriorityClass> set_priority(const ProcessKey& process,
                                                           PriorityClass priority) = 0;

  [[nodiscard]] virtual Result<MemoryPriority> memory_priority(const ProcessKey& process) const = 0;
  [[nodiscard]] virtual Result<MemoryPriority> set_memory_priority(const ProcessKey& process,
                                                                   MemoryPriority priority) = 0;

  // Only the execution-speed bit is read or changed; other throttling bits are kept.
  [[nodiscard]] virtual Result<EcoQos> eco_qos(const ProcessKey& process) const = 0;
  [[nodiscard]] virtual Result<EcoQos> set_eco_qos(const ProcessKey& process, EcoQos state) = 0;

  // Suspends are counted: n suspends need n resumes. A suspend outlives the caller, and a resume
  // also releases suspends someone else made (spike X4 §3.6), so the caller resumes only what it
  // recorded.
  [[nodiscard]] virtual Result<void> suspend(const ProcessKey& process) = 0;
  [[nodiscard]] virtual Result<void> resume(const ProcessKey& process) = 0;

  [[nodiscard]] virtual Result<void> terminate(const ProcessKey& process,
                                               std::uint32_t exit_code) = 0;

 protected:
  IProcessControl() = default;
};

}  // namespace myclean::os
