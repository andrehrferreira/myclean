#include "platform/jobs/jobs_support.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <ratio>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"

namespace myclean::platform::jobs_support {

namespace {

// FILETIME units: 100 ns since 1601-01-01.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
constexpr std::int64_t unix_epoch_ticks = 116'444'736'000'000'000;

constexpr auto status_access_denied = static_cast<LONG>(0xC0000022L);
constexpr auto status_object_name_not_found = static_cast<LONG>(0xC0000034L);
constexpr auto status_object_name_collision = static_cast<LONG>(0xC0000035L);
constexpr auto status_privilege_not_held = static_cast<LONG>(0xC0000061L);
constexpr auto status_process_is_terminating = static_cast<LONG>(0xC000010AL);

template <typename Function>
Function export_of(HMODULE module, const char* name) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
  return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

}  // namespace

const Native& native() {
  static const Native functions = [] {
    Native found;
    if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
      found.create_job = export_of<Native::CreateJob>(ntdll, "NtCreateJobObject");
      found.make_temporary = export_of<Native::HandleCall>(ntdll, "NtMakeTemporaryObject");
      found.suspend_process = export_of<Native::HandleCall>(ntdll, "NtSuspendProcess");
      found.resume_process = export_of<Native::HandleCall>(ntdll, "NtResumeProcess");
      found.query_object = export_of<Native::QueryObject>(ntdll, "NtQueryObject");
    }
    return found;
  }();
  return functions;
}

tl::unexpected<Error> win32_error(std::string_view what, DWORD error) {
  Errc code = Errc::io_error;
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      code = Errc::not_found;
      break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
      code = Errc::access_denied;
      break;
    case ERROR_ALREADY_EXISTS:
      code = Errc::already_exists;
      break;
    case ERROR_INVALID_PARAMETER:
      code = Errc::invalid_argument;
      break;
    default:
      break;
  }
  return fail(code, std::format("{}: Win32 error {}", what, error));
}

tl::unexpected<Error> nt_error(std::string_view what, LONG status) {
  Errc code = Errc::io_error;
  switch (status) {
    case status_access_denied:
    case status_privilege_not_held:
      code = Errc::access_denied;
      break;
    case status_object_name_not_found:
    case status_process_is_terminating:
      code = Errc::not_found;
      break;
    case status_object_name_collision:
      code = Errc::already_exists;
      break;
    default:
      break;
  }
  return fail(code, std::format("{}: NTSTATUS {:#010x}", what, static_cast<std::uint32_t>(status)));
}

WallTime wall_time_of(const FILETIME& time) {
  const auto ticks =
      static_cast<std::int64_t>((std::uint64_t{time.dwHighDateTime} << 32U) | time.dwLowDateTime);
  return WallTime{std::chrono::duration_cast<WallTime::duration>(Ticks{ticks - unix_epoch_ticks})};
}

Result<WallTime> creation_time(HANDLE process, std::uint32_t pid) {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
    return win32_error(std::format("GetProcessTimes(pid {})", pid), ::GetLastError());
  }
  return wall_time_of(created);
}

Result<wil::unique_handle> open_process(const os::ProcessKey& process, DWORD access) {
  wil::unique_handle handle{
      ::OpenProcess(access | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid)};
  if (!handle) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
      return fail(Errc::not_found, std::format("OpenProcess(pid {}): no such process (Win32 "
                                               "error {})",
                                               process.pid, error));
    }
    return win32_error(std::format("OpenProcess(pid {})", process.pid), error);
  }
  const Result<WallTime> created = creation_time(handle.get(), process.pid);
  if (!created) {
    return tl::unexpected(created.error());
  }
  if (*created != process.created) {
    return fail(Errc::not_found,
                std::format("pid {} now belongs to another process (created {} instead of {})",
                            process.pid, *created, process.created));
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(handle.get(), &exit_code) == FALSE) {
    return win32_error(std::format("GetExitCodeProcess(pid {})", process.pid), ::GetLastError());
  }
  if (exit_code != STILL_ACTIVE) {
    return fail(Errc::not_found,
                std::format("pid {} has exited with code {:#x}", process.pid, exit_code));
  }
  return handle;
}

}  // namespace myclean::platform::jobs_support
