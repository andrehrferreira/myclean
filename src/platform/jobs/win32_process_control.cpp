#include "platform/jobs/win32_process_control.hpp"

#include <cstdint>
#include <format>
#include <string>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/os/process_control.hpp"
#include "platform/jobs/jobs_support.hpp"

namespace myclean::platform {

namespace {

using jobs_support::open_process;
using jobs_support::win32_error;

Result<os::PriorityClass> priority_of(DWORD priority_class, std::uint32_t pid) {
  switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
      return os::PriorityClass::idle;
    case BELOW_NORMAL_PRIORITY_CLASS:
      return os::PriorityClass::below_normal;
    case NORMAL_PRIORITY_CLASS:
      return os::PriorityClass::normal;
    case ABOVE_NORMAL_PRIORITY_CLASS:
      return os::PriorityClass::above_normal;
    case HIGH_PRIORITY_CLASS:
      return os::PriorityClass::high;
    case REALTIME_PRIORITY_CLASS:
      return os::PriorityClass::realtime;
    default:
      return fail(Errc::invalid_data, std::format("GetPriorityClass(pid {}): unknown class {:#x}",
                                                  pid, priority_class));
  }
}

DWORD priority_class_of(os::PriorityClass priority) {
  switch (priority) {
    case os::PriorityClass::idle:
      return IDLE_PRIORITY_CLASS;
    case os::PriorityClass::below_normal:
      return BELOW_NORMAL_PRIORITY_CLASS;
    case os::PriorityClass::above_normal:
      return ABOVE_NORMAL_PRIORITY_CLASS;
    case os::PriorityClass::high:
      return HIGH_PRIORITY_CLASS;
    case os::PriorityClass::realtime:
      return REALTIME_PRIORITY_CLASS;
    case os::PriorityClass::normal:
      break;
  }
  return NORMAL_PRIORITY_CLASS;
}

Result<os::PriorityClass> read_priority(HANDLE process, std::uint32_t pid) {
  const DWORD priority_class = ::GetPriorityClass(process);
  if (priority_class == 0) {
    return win32_error(std::format("GetPriorityClass(pid {})", pid), ::GetLastError());
  }
  return priority_of(priority_class, pid);
}

Result<os::MemoryPriority> read_memory_priority(HANDLE process, std::uint32_t pid) {
  MEMORY_PRIORITY_INFORMATION info{};
  if (::GetProcessInformation(process, ProcessMemoryPriority, &info, sizeof(info)) == FALSE) {
    return win32_error(std::format("GetProcessInformation(pid {}, ProcessMemoryPriority)", pid),
                       ::GetLastError());
  }
  if (info.MemoryPriority < MEMORY_PRIORITY_VERY_LOW ||
      info.MemoryPriority > MEMORY_PRIORITY_NORMAL) {
    return fail(Errc::invalid_data,
                std::format("pid {}: unknown memory priority {}", pid, info.MemoryPriority));
  }
  return static_cast<os::MemoryPriority>(info.MemoryPriority);
}

Result<PROCESS_POWER_THROTTLING_STATE> read_throttling(HANDLE process, std::uint32_t pid) {
  PROCESS_POWER_THROTTLING_STATE state{.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION};
  if (::GetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state)) == FALSE) {
    return win32_error(std::format("GetProcessInformation(pid {}, ProcessPowerThrottling)", pid),
                       ::GetLastError());
  }
  return state;
}

os::EcoQos eco_qos_of(const PROCESS_POWER_THROTTLING_STATE& state) {
  if ((state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0) {
    return os::EcoQos::automatic;
  }
  return (state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0 ? os::EcoQos::on
                                                                           : os::EcoQos::off;
}

}  // namespace

Result<os::PriorityClass> Win32ProcessControl::priority(const os::ProcessKey& process) const {
  return open_process(process, 0).and_then([&](const wil::unique_handle& handle) {
    return read_priority(handle.get(), process.pid);
  });
}

Result<os::PriorityClass> Win32ProcessControl::set_priority(const os::ProcessKey& process,
                                                            os::PriorityClass priority) {
  Result<wil::unique_handle> handle = open_process(process, PROCESS_SET_INFORMATION);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  Result<os::PriorityClass> previous = read_priority(handle->get(), process.pid);
  if (!previous) {
    return previous;
  }
  if (::SetPriorityClass(handle->get(), priority_class_of(priority)) == FALSE) {
    return win32_error(std::format("SetPriorityClass(pid {})", process.pid), ::GetLastError());
  }
  return previous;
}

Result<os::MemoryPriority> Win32ProcessControl::memory_priority(
    const os::ProcessKey& process) const {
  return open_process(process, 0).and_then([&](const wil::unique_handle& handle) {
    return read_memory_priority(handle.get(), process.pid);
  });
}

Result<os::MemoryPriority> Win32ProcessControl::set_memory_priority(const os::ProcessKey& process,
                                                                    os::MemoryPriority priority) {
  Result<wil::unique_handle> handle = open_process(process, PROCESS_SET_INFORMATION);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  Result<os::MemoryPriority> previous = read_memory_priority(handle->get(), process.pid);
  if (!previous) {
    return previous;
  }
  MEMORY_PRIORITY_INFORMATION info{.MemoryPriority = static_cast<ULONG>(priority)};
  if (::SetProcessInformation(handle->get(), ProcessMemoryPriority, &info, sizeof(info)) == FALSE) {
    return win32_error(
        std::format("SetProcessInformation(pid {}, ProcessMemoryPriority)", process.pid),
        ::GetLastError());
  }
  return previous;
}

Result<os::EcoQos> Win32ProcessControl::eco_qos(const os::ProcessKey& process) const {
  return open_process(process, 0)
      .and_then([&](const wil::unique_handle& handle) {
        return read_throttling(handle.get(), process.pid);
      })
      .transform(eco_qos_of);
}

Result<os::EcoQos> Win32ProcessControl::set_eco_qos(const os::ProcessKey& process,
                                                    os::EcoQos state) {
  Result<wil::unique_handle> handle = open_process(process, PROCESS_SET_INFORMATION);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  Result<PROCESS_POWER_THROTTLING_STATE> current = read_throttling(handle->get(), process.pid);
  if (!current) {
    return tl::unexpected(current.error());
  }
  PROCESS_POWER_THROTTLING_STATE next = *current;
  next.ControlMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  next.StateMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  if (state != os::EcoQos::automatic) {
    next.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  }
  if (state == os::EcoQos::on) {
    next.StateMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  }
  if (::SetProcessInformation(handle->get(), ProcessPowerThrottling, &next, sizeof(next)) ==
      FALSE) {
    return win32_error(
        std::format("SetProcessInformation(pid {}, ProcessPowerThrottling)", process.pid),
        ::GetLastError());
  }
  return eco_qos_of(*current);
}

Result<void> Win32ProcessControl::suspend(const os::ProcessKey& process) {
  const auto call = jobs_support::native().suspend_process;
  if (call == nullptr) {
    return fail(Errc::io_error, "NtSuspendProcess is not exported by ntdll");
  }
  Result<wil::unique_handle> handle = open_process(process, PROCESS_SUSPEND_RESUME);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  if (const LONG status = call(handle->get()); status < 0) {
    return jobs_support::nt_error(std::format("NtSuspendProcess(pid {})", process.pid), status);
  }
  return {};
}

Result<void> Win32ProcessControl::resume(const os::ProcessKey& process) {
  const auto call = jobs_support::native().resume_process;
  if (call == nullptr) {
    return fail(Errc::io_error, "NtResumeProcess is not exported by ntdll");
  }
  Result<wil::unique_handle> handle = open_process(process, PROCESS_SUSPEND_RESUME);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  if (const LONG status = call(handle->get()); status < 0) {
    return jobs_support::nt_error(std::format("NtResumeProcess(pid {})", process.pid), status);
  }
  return {};
}

Result<void> Win32ProcessControl::terminate(const os::ProcessKey& process,
                                            std::uint32_t exit_code) {
  Result<wil::unique_handle> handle = open_process(process, PROCESS_TERMINATE);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  if (::TerminateProcess(handle->get(), exit_code) == FALSE) {
    return win32_error(std::format("TerminateProcess(pid {})", process.pid), ::GetLastError());
  }
  return {};
}

Result<os::ProcessKey> process_key(std::uint32_t pid) {
  const wil::unique_handle handle{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!handle) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
      return fail(Errc::not_found, std::format("OpenProcess(pid {}): no such process", pid));
    }
    return win32_error(std::format("OpenProcess(pid {})", pid), error);
  }
  return jobs_support::creation_time(handle.get(), pid).transform([&](WallTime created) {
    return os::ProcessKey{.pid = pid, .created = created};
  });
}

}  // namespace myclean::platform
