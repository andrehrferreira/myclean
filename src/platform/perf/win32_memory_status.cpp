#include "platform/perf/win32_memory_status.hpp"

#include <cstddef>
#include <string>

// clang-format off
#include <windows.h>  // first: psapi.h needs its types
#include <psapi.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/memory_status.hpp"

namespace myclean::platform {

Result<telemetry::MemoryStatus> Win32MemoryStatus::read() const {
  PERFORMANCE_INFORMATION info{.cb = sizeof(PERFORMANCE_INFORMATION)};
  if (::GetPerformanceInfo(&info, sizeof(info)) == FALSE) {
    return fail(Errc::io_error,
                "GetPerformanceInfo failed: Win32 error " + std::to_string(::GetLastError()));
  }
  const auto pages = [&](std::size_t count) { return Bytes{count * info.PageSize}; };
  return telemetry::MemoryStatus{.total = pages(info.PhysicalTotal),
                                 .available = pages(info.PhysicalAvailable),
                                 .commit = pages(info.CommitTotal),
                                 .commit_limit = pages(info.CommitLimit),
                                 .pool_paged = pages(info.KernelPaged),
                                 .pool_nonpaged = pages(info.KernelNonpaged)};
}

}  // namespace myclean::platform
