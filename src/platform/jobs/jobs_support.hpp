#pragma once

// Helpers shared by the job and process adapters: run-time resolved ntdll functions, Win32 and
// NTSTATUS failures as Results, and processes opened by verified (PID, creation time).

#include <cstdint>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"

namespace myclean::platform::jobs_support {

// Native APIs resolved from ntdll at run time: a binary that imports NtSuspendProcess statically
// was quarantined by the antivirus on the owner's machine (spike X4 §3.8). A missing export is
// null; the caller reports it.
struct Native {
  using CreateJob = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, void*);
  using HandleCall = LONG(NTAPI*)(HANDLE);
  using QueryObject = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

  CreateJob create_job = nullptr;        // NtCreateJobObject
  HandleCall make_temporary = nullptr;   // NtMakeTemporaryObject
  HandleCall suspend_process = nullptr;  // NtSuspendProcess
  HandleCall resume_process = nullptr;   // NtResumeProcess
  QueryObject query_object = nullptr;    // NtQueryObject
};

[[nodiscard]] const Native& native();

// `what` (the API and its subject) failed with Win32 error `error`, in the Errc it belongs to.
[[nodiscard]] tl::unexpected<Error> win32_error(std::string_view what, DWORD error);
// `what` failed with NTSTATUS `status`, in the Errc it belongs to.
[[nodiscard]] tl::unexpected<Error> nt_error(std::string_view what, LONG status);

[[nodiscard]] WallTime wall_time_of(const FILETIME& time);

// The creation time of an open process (PROCESS_QUERY_LIMITED_INFORMATION).
[[nodiscard]] Result<WallTime> creation_time(HANDLE process, std::uint32_t pid);

// Opens `process` with `access` plus PROCESS_QUERY_LIMITED_INFORMATION and checks that it is
// still that process: not_found when the PID is gone, belongs to a process with another creation
// time (PID reuse), or the process has exited.
[[nodiscard]] Result<wil::unique_handle> open_process(const os::ProcessKey& process, DWORD access);

}  // namespace myclean::platform::jobs_support
