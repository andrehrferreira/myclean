#pragma once

#include <cstdint>

#include "core/model/result.hpp"
#include "core/os/process_control.hpp"

namespace myclean::platform {

// The governor's per-process actions through the documented process APIs (priority class,
// ProcessMemoryPriority, ProcessPowerThrottling, TerminateProcess) and NtSuspendProcess /
// NtResumeProcess, resolved from ntdll at run time (spike X4 §3.8). Every call opens the process
// with only the rights it needs and verifies its creation time first.
class Win32ProcessControl final : public os::IProcessControl {
 public:
  Win32ProcessControl() = default;

  [[nodiscard]] Result<os::PriorityClass> priority(const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::PriorityClass> set_priority(const os::ProcessKey& process,
                                                       os::PriorityClass priority) override;
  [[nodiscard]] Result<os::MemoryPriority> memory_priority(
      const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::MemoryPriority> set_memory_priority(
      const os::ProcessKey& process, os::MemoryPriority priority) override;
  [[nodiscard]] Result<os::EcoQos> eco_qos(const os::ProcessKey& process) const override;
  [[nodiscard]] Result<os::EcoQos> set_eco_qos(const os::ProcessKey& process,
                                               os::EcoQos state) override;
  [[nodiscard]] Result<void> suspend(const os::ProcessKey& process) override;
  [[nodiscard]] Result<void> resume(const os::ProcessKey& process) override;
  [[nodiscard]] Result<void> terminate(const os::ProcessKey& process,
                                       std::uint32_t exit_code) override;
};

// The key of the live process `pid`: its PID and current creation time. not_found when no such
// process runs.
[[nodiscard]] Result<os::ProcessKey> process_key(std::uint32_t pid);

}  // namespace myclean::platform
