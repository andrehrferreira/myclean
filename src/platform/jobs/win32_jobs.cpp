#include "platform/jobs/win32_jobs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <ratio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <aclapi.h>
#include <sddl.h>
#include <winternl.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "platform/jobs/jobs_support.hpp"

namespace myclean::platform {

namespace {

using jobs_support::native;
using jobs_support::nt_error;
using jobs_support::open_process;
using jobs_support::win32_error;

// FILETIME units in accounting.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

constexpr DWORD job_access = JOB_OBJECT_ALL_ACCESS;
constexpr ULONG obj_case_insensitive = 0x40;
constexpr ULONG obj_permanent = 0x10;
constexpr DWORD cpu_hard_cap =
    JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
constexpr DWORD cpu_other_modes =
    JOB_OBJECT_CPU_RATE_CONTROL_WEIGHT_BASED | JOB_OBJECT_CPU_RATE_CONTROL_MIN_MAX_RATE;
constexpr double max_cpu_rate = 10'000;  // CpuRate is in 1/100 of a percent
constexpr std::size_t port_batch = 64;
constexpr std::size_t initial_pid_capacity = 64;

std::wstring widen_ascii(std::string_view text) {
  return {text.begin(), text.end()};
}

std::string narrow_ascii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const wchar_t c : text) {
    result.push_back(static_cast<char>(c));
  }
  return result;
}

std::string random_nonce() {
  std::random_device device;  // rand_s on MSVC: the system's cryptographic generator
  std::string nonce;
  for (int i = 0; i < 4; ++i) {
    nonce += std::format("{:08x}", device());
  }
  return nonce;
}

// The TOKEN_USER of this process, in a buffer that keeps the SID alive.
Result<std::vector<std::byte>> token_user() {
  wil::unique_handle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
    return win32_error("OpenProcessToken", ::GetLastError());
  }
  DWORD size = 0;
  ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  std::vector<std::byte> buffer(size);
  if (::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE) {
    return win32_error("GetTokenInformation(TokenUser)", ::GetLastError());
  }
  return buffer;
}

PSID user_sid_of(std::vector<std::byte>& user) {
  TOKEN_USER header{};
  std::memcpy(&header, user.data(), sizeof(header));
  return header.User.Sid;
}

Result<std::string> sid_string(PSID sid) {
  wil::unique_hlocal_string text;
  if (::ConvertSidToStringSidW(sid, &text) == FALSE) {
    return win32_error("ConvertSidToStringSidW", ::GetLastError());
  }
  return narrow_ascii(text.get());
}

// Enables `privilege` in this process's token; false when the token does not hold it.
bool enable_privilege(const wchar_t* privilege) {
  wil::unique_handle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) ==
      FALSE) {
    return false;
  }
  TOKEN_PRIVILEGES change{.PrivilegeCount = 1};
  if (::LookupPrivilegeValueW(nullptr, privilege, &change.Privileges[0].Luid) == FALSE) {
    return false;
  }
  change.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (::AdjustTokenPrivileges(token.get(), FALSE, &change, 0, nullptr, nullptr) == FALSE) {
    return false;
  }
  return ::GetLastError() == ERROR_SUCCESS;  // ERROR_NOT_ALL_ASSIGNED: not held
}

template <typename Info>
Result<Info> query(HANDLE job, JOBOBJECTINFOCLASS info_class, std::string_view what) {
  Info info{};
  if (::QueryInformationJobObject(job, info_class, &info, sizeof(info), nullptr) == FALSE) {
    return win32_error(std::format("QueryInformationJobObject({})", what), ::GetLastError());
  }
  return info;
}

template <typename Info>
Result<void> set(HANDLE job, JOBOBJECTINFOCLASS info_class, Info info, std::string_view what) {
  if (::SetInformationJobObject(job, info_class, &info, sizeof(info)) == FALSE) {
    return win32_error(std::format("SetInformationJobObject({})", what), ::GetLastError());
  }
  return {};
}

// True when `object` is a permanent object (ObjectBasicInformation reports OBJ_PERMANENT).
Result<bool> is_permanent(HANDLE object, std::string_view name) {
  const auto query_object = native().query_object;
  if (query_object == nullptr) {
    return fail(Errc::io_error, "NtQueryObject is not exported by ntdll");
  }
  PUBLIC_OBJECT_BASIC_INFORMATION info{};
  if (const LONG status =
          query_object(object, ObjectBasicInformation, &info, sizeof(info), nullptr);
      status < 0) {
    return nt_error(std::format("NtQueryObject({}, ObjectBasicInformation)", name), status);
  }
  return (info.Attributes & obj_permanent) != 0;
}

Result<wil::unique_handle> attach_port(HANDLE job) {
  wil::unique_handle port{::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)};
  if (!port) {
    return win32_error("CreateIoCompletionPort", ::GetLastError());
  }
  const JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{.CompletionKey = nullptr,
                                                        .CompletionPort = port.get()};
  if (Result<void> attached = set(job, JobObjectAssociateCompletionPortInformation, association,
                                  "AssociateCompletionPort");
      !attached) {
    return tl::unexpected(attached.error());
  }
  return port;
}

class Win32Job final : public os::IJob {
 public:
  Win32Job(os::JobRef ref, wil::unique_handle job, wil::unique_handle port, bool permanent)
      : ref_(std::move(ref)), job_(std::move(job)), port_(std::move(port)), permanent_(permanent) {}

  [[nodiscard]] const os::JobRef& ref() const override { return ref_; }
  [[nodiscard]] bool permanent() const override { return permanent_; }
  [[nodiscard]] bool has_port() const override { return static_cast<bool>(port_); }

  [[nodiscard]] Result<void> assign(const os::ProcessKey& process) override {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    Result<wil::unique_handle> handle =
        open_process(process, PROCESS_SET_QUOTA | PROCESS_TERMINATE);
    if (!handle) {
      return tl::unexpected(handle.error());
    }
    BOOL member = FALSE;
    if (::IsProcessInJob(handle->get(), job_.get(), &member) == FALSE) {
      return win32_error(std::format("IsProcessInJob(pid {})", process.pid), ::GetLastError());
    }
    if (member != FALSE) {
      return {};
    }
    if (::AssignProcessToJobObject(job_.get(), handle->get()) == FALSE) {
      return win32_error(
          std::format("AssignProcessToJobObject(pid {}, {})", process.pid, ref_.object_name()),
          ::GetLastError());
    }
    return {};
  }

  [[nodiscard]] Result<bool> contains(const os::ProcessKey& process) const override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    Result<wil::unique_handle> handle = open_process(process, 0);
    if (!handle) {
      return tl::unexpected(handle.error());
    }
    BOOL member = FALSE;
    if (::IsProcessInJob(handle->get(), job_.get(), &member) == FALSE) {
      return win32_error(std::format("IsProcessInJob(pid {})", process.pid), ::GetLastError());
    }
    return member != FALSE;
  }

  [[nodiscard]] Result<void> set_cpu_cap(double percent) override {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    if (std::isnan(percent) || percent <= 0.0 || percent > 100.0) {
      return fail(Errc::invalid_argument,
                  std::format("CPU cap must be above 0 and at most 100 %: {}", percent));
    }
    const double rate = std::clamp(std::round(percent * 100.0), 1.0, max_cpu_rate);
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info{};
    info.ControlFlags = cpu_hard_cap;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Win32 struct, hard-cap member
    info.CpuRate = static_cast<DWORD>(rate);
    return set(job_.get(), JobObjectCpuRateControlInformation, info, "CpuRate hard cap");
  }

  [[nodiscard]] Result<void> lift_cpu_cap() override {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    return set(job_.get(), JobObjectCpuRateControlInformation,
               JOBOBJECT_CPU_RATE_CONTROL_INFORMATION{}, "CpuRate off");
  }

  [[nodiscard]] Result<void> set_commit_cap(Bytes cap) override {
    if (cap.count() == 0) {
      return fail(Errc::invalid_argument, "commit cap must be non-zero");
    }
    return change_extended([&](JOBOBJECT_EXTENDED_LIMIT_INFORMATION& info) {
      info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
      info.JobMemoryLimit = static_cast<SIZE_T>(cap.count());
    });
  }

  [[nodiscard]] Result<void> lift_commit_cap() override {
    return change_extended([](JOBOBJECT_EXTENDED_LIMIT_INFORMATION& info) {
      info.BasicLimitInformation.LimitFlags &= ~static_cast<DWORD>(JOB_OBJECT_LIMIT_JOB_MEMORY);
      info.JobMemoryLimit = 0;
    });
  }

  [[nodiscard]] Result<void> set_commit_notification(Bytes limit) override {
    if (limit.count() == 0) {
      return fail(Errc::invalid_argument, "commit notification limit must be non-zero");
    }
    return change_notification([&](JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION& info) {
      info.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
      info.JobMemoryLimit = limit.count();
    });
  }

  [[nodiscard]] Result<void> clear_commit_notification() override {
    return change_notification([](JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION& info) {
      info.LimitFlags &= ~static_cast<DWORD>(JOB_OBJECT_LIMIT_JOB_MEMORY);
      info.JobMemoryLimit = 0;
    });
  }

  // Tries every part even when one fails, so as much as possible is lifted; reports the first
  // failure.
  [[nodiscard]] Result<void> lift_all() override {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    const std::array<Result<void>, 3> results = {
        set(job_.get(), JobObjectCpuRateControlInformation,
            JOBOBJECT_CPU_RATE_CONTROL_INFORMATION{}, "CpuRate off"),
        set(job_.get(), JobObjectExtendedLimitInformation, JOBOBJECT_EXTENDED_LIMIT_INFORMATION{},
            "no limit flags"),
        set(job_.get(), JobObjectNotificationLimitInformation,
            JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION{}, "no notification limit"),
    };
    for (const Result<void>& result : results) {
      if (!result) {
        return result;
      }
    }
    return {};
  }

  [[nodiscard]] Result<os::JobLimits> limits() const override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    const auto cpu = query<JOBOBJECT_CPU_RATE_CONTROL_INFORMATION>(
        job_.get(), JobObjectCpuRateControlInformation, "CpuRate");
    if (!cpu) {
      return tl::unexpected(cpu.error());
    }
    const auto extended = query<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>(
        job_.get(), JobObjectExtendedLimitInformation, "ExtendedLimit");
    if (!extended) {
      return tl::unexpected(extended.error());
    }
    const auto notification = query<JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION>(
        job_.get(), JobObjectNotificationLimitInformation, "NotificationLimit");
    if (!notification) {
      return tl::unexpected(notification.error());
    }
    os::JobLimits limits;
    if ((cpu->ControlFlags & cpu_hard_cap) == cpu_hard_cap &&
        (cpu->ControlFlags & cpu_other_modes) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): Win32 struct, hard-cap member
      limits.cpu_cap_percent = static_cast<double>(cpu->CpuRate) / 100.0;
    }
    if ((extended->BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0) {
      limits.commit_cap = Bytes{extended->JobMemoryLimit};
    }
    if ((notification->LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0) {
      limits.commit_notification = Bytes{notification->JobMemoryLimit};
    }
    return limits;
  }

  [[nodiscard]] Result<std::vector<std::uint32_t>> members() const override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    // JOBOBJECT_BASIC_PROCESS_ID_LIST: two DWORD counts, then the ULONG_PTR ids.
    static_assert(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) == 2 * sizeof(ULONG_PTR));
    std::size_t capacity = initial_pid_capacity;
    while (true) {
      std::vector<ULONG_PTR> buffer(capacity + 1);
      const auto bytes = static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR));
      const BOOL ok = ::QueryInformationJobObject(job_.get(), JobObjectBasicProcessIdList,
                                                  buffer.data(), bytes, nullptr);
      const DWORD error = ok == FALSE ? ::GetLastError() : ERROR_SUCCESS;
      if (ok == FALSE && error != ERROR_MORE_DATA) {
        return win32_error("QueryInformationJobObject(BasicProcessIdList)", error);
      }
      std::array<DWORD, 2> counts{};  // NumberOfAssignedProcesses, NumberOfProcessIdsInList
      std::memcpy(counts.data(), buffer.data(), sizeof(counts));
      if (ok == FALSE || counts[1] < counts[0]) {
        capacity = std::max<std::size_t>(capacity * 2, counts[0] + 16);
        continue;
      }
      std::vector<std::uint32_t> pids;
      pids.reserve(counts[1]);
      for (std::size_t i = 0; i < counts[1]; ++i) {
        pids.push_back(static_cast<std::uint32_t>(buffer[i + 1]));
      }
      return pids;
    }
  }

  [[nodiscard]] Result<os::JobAccounting> accounting() const override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    const auto basic = query<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION>(
        job_.get(), JobObjectBasicAccountingInformation, "BasicAccounting");
    if (!basic) {
      return tl::unexpected(basic.error());
    }
    const auto extended = query<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>(
        job_.get(), JobObjectExtendedLimitInformation, "ExtendedLimit");
    if (!extended) {
      return tl::unexpected(extended.error());
    }
    const Result<os::LimitViolation> violation = limit_violation();
    if (!violation) {
      return tl::unexpected(violation.error());
    }
    return os::JobAccounting{
        .cpu_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Ticks{basic->TotalUserTime.QuadPart + basic->TotalKernelTime.QuadPart}),
        .committed = violation->committed,
        .peak_committed = Bytes{extended->PeakJobMemoryUsed},
        .active_processes = basic->ActiveProcesses,
        .total_processes = basic->TotalProcesses,
    };
  }

  [[nodiscard]] Result<os::LimitViolation> limit_violation() const override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    const auto info = query<JOBOBJECT_LIMIT_VIOLATION_INFORMATION>(
        job_.get(), JobObjectLimitViolationInformation, "LimitViolation");
    if (!info) {
      return tl::unexpected(info.error());
    }
    return os::LimitViolation{
        .commit_exceeded = (info->ViolationLimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0,
        .committed = Bytes{info->JobMemory},
        .notification_limit =
            Bytes{(info->LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0 ? info->JobMemoryLimit
                                                                        : 0U},
    };
  }

  [[nodiscard]] Result<std::vector<os::JobEvent>> poll(std::size_t max_messages) override {
    if (Result<void> open = check_open(); !open) {
      return tl::unexpected(open.error());
    }
    if (!port_) {
      return fail(Errc::invalid_state,
                  "job " + ref_.object_name() + " has no completion port; poll members()");
    }
    std::vector<os::JobEvent> events;
    std::optional<std::size_t> notification;  // index of the folded notification event
    std::array<OVERLAPPED_ENTRY, port_batch> entries{};
    std::size_t drained = 0;
    while (drained < max_messages) {
      const auto wanted = static_cast<ULONG>(std::min(port_batch, max_messages - drained));
      ULONG removed = 0;
      if (::GetQueuedCompletionStatusEx(port_.get(), entries.data(), wanted, &removed, 0, FALSE) ==
          FALSE) {
        const DWORD error = ::GetLastError();
        if (error == WAIT_TIMEOUT) {
          break;
        }
        return win32_error("GetQueuedCompletionStatusEx", error);
      }
      for (std::size_t i = 0; i < removed; ++i) {
        translate(entries.at(i), events, notification);
      }
      drained += removed;
      if (removed < wanted) {
        break;
      }
    }
    if (notification) {
      if (Result<os::LimitViolation> violation = limit_violation(); violation) {
        events[*notification].violation = *violation;
      }
    }
    return events;
  }

  [[nodiscard]] Result<void> release() override {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    const auto make_temporary = native().make_temporary;
    if (make_temporary == nullptr) {
      return fail(Errc::io_error, "NtMakeTemporaryObject is not exported by ntdll");
    }
    if (const LONG status = make_temporary(job_.get()); status < 0) {
      return nt_error("NtMakeTemporaryObject(" + ref_.object_name() + ")", status);
    }
    watched_.clear();
    port_.reset();
    job_.reset();
    return {};
  }

 private:
  // A member seen by new_process, kept open so its exit code and creation time stay readable.
  struct Watched {
    wil::unique_handle handle;
    WallTime created;
  };

  [[nodiscard]] Result<void> check_open() const {
    if (!job_) {
      return fail(Errc::invalid_state, "job " + ref_.object_name() + " was released");
    }
    return {};
  }

  template <typename Change>
  Result<void> change_extended(Change change) {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    auto info = query<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>(
        job_.get(), JobObjectExtendedLimitInformation, "ExtendedLimit");
    if (!info) {
      return tl::unexpected(info.error());
    }
    change(*info);
    return set(job_.get(), JobObjectExtendedLimitInformation, *info, "ExtendedLimit");
  }

  template <typename Change>
  Result<void> change_notification(Change change) {
    if (Result<void> open = check_open(); !open) {
      return open;
    }
    auto info = query<JOBOBJECT_NOTIFICATION_LIMIT_INFORMATION>(
        job_.get(), JobObjectNotificationLimitInformation, "NotificationLimit");
    if (!info) {
      return tl::unexpected(info.error());
    }
    change(*info);
    return set(job_.get(), JobObjectNotificationLimitInformation, *info, "NotificationLimit");
  }

  // Opens `pid` when it is (still) a member of this job; a reused PID is not.
  [[nodiscard]] std::optional<Watched> open_member(std::uint32_t pid) const {
    wil::unique_handle handle{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    BOOL member = FALSE;
    if (!handle || ::IsProcessInJob(handle.get(), job_.get(), &member) == FALSE ||
        member == FALSE) {
      return std::nullopt;
    }
    Result<WallTime> created = jobs_support::creation_time(handle.get(), pid);
    if (!created) {
      return std::nullopt;
    }
    return Watched{.handle = std::move(handle), .created = *created};
  }

  void on_new_process(std::uint32_t pid, os::JobEvent& event) {
    std::optional<Watched> member = open_member(pid);
    if (member) {
      event.created = member->created;
      watched_.insert_or_assign(pid, std::move(*member));
    }
  }

  void on_exit_process(std::uint32_t pid, os::JobEvent& event) {
    std::optional<Watched> member;
    if (auto found = watched_.find(pid); found != watched_.end()) {
      member = std::move(found->second);
      watched_.erase(found);
    } else {
      member = open_member(pid);
    }
    DWORD exit_code = 0;
    if (!member || ::GetExitCodeProcess(member->handle.get(), &exit_code) == FALSE ||
        exit_code == STILL_ACTIVE) {
      return;  // gone without a handle, or a new member that reuses the PID
    }
    event.created = member->created;
    event.exit_code = exit_code;
    event.commit_failure = os::is_commit_failure_exit(exit_code);
  }

  void translate(const OVERLAPPED_ENTRY& entry, std::vector<os::JobEvent>& events,
                 std::optional<std::size_t>& notification) {
    // The port message is the "bytes transferred" field; the PID travels in the OVERLAPPED
    // pointer.
    const DWORD message = entry.dwNumberOfBytesTransferred;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): job ports carry the PID here
    const auto pid = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(entry.lpOverlapped));
    os::JobEvent event;
    switch (message) {
      case JOB_OBJECT_MSG_NEW_PROCESS:
        event.kind = os::JobEventKind::new_process;
        event.pid = pid;
        on_new_process(pid, event);
        break;
      case JOB_OBJECT_MSG_EXIT_PROCESS:
      case JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS:
        event.kind = os::JobEventKind::exit_process;
        event.pid = pid;
        event.abnormal = message == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS;
        on_exit_process(pid, event);
        break;
      case JOB_OBJECT_MSG_JOB_MEMORY_LIMIT:
        event.kind = os::JobEventKind::job_memory_limit;
        event.pid = pid;
        break;
      case JOB_OBJECT_MSG_NOTIFICATION_LIMIT:
        if (notification) {
          ++events[*notification].count;
          return;
        }
        event.kind = os::JobEventKind::notification_limit;
        notification = events.size();
        break;
      case JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO:
        event.kind = os::JobEventKind::active_process_zero;
        break;
      default:
        return;  // limits MyClean never sets (time, process count, per-process memory)
    }
    events.push_back(event);
  }

  os::JobRef ref_;
  wil::unique_handle job_;
  wil::unique_handle port_;
  bool permanent_;
  std::unordered_map<std::uint32_t, Watched> watched_;
};

}  // namespace

Win32JobControl::Win32JobControl(std::string expected_owner)
    : expected_owner_(std::move(expected_owner)),
      permanent_(enable_privilege(SE_CREATE_PERMANENT_NAME)) {}

Result<std::unique_ptr<os::IJob>> Win32JobControl::create(std::string_view job_class) {
  if (Result<void> valid = os::validate_job_class(job_class); !valid) {
    return tl::unexpected(valid.error());
  }
  const auto create_job = native().create_job;
  if (create_job == nullptr) {
    return fail(Errc::io_error, "NtCreateJobObject is not exported by ntdll");
  }
  os::JobRef ref{.job_class = std::string(job_class), .nonce = random_nonce()};

  // Owner: this process's user, explicitly. The DACL is the default one, as for a job created
  // without security attributes.
  Result<std::vector<std::byte>> user = token_user();
  if (!user) {
    return tl::unexpected(user.error());
  }
  SECURITY_DESCRIPTOR descriptor{};
  if (::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
      ::SetSecurityDescriptorOwner(&descriptor, user_sid_of(*user), FALSE) == FALSE) {
    return win32_error("SetSecurityDescriptorOwner", ::GetLastError());
  }

  // Global\ is \BaseNamedObjects for the object manager.
  std::wstring name = L"\\BaseNamedObjects\\MyClean.Job." + widen_ascii(ref.job_class) + L"." +
                      widen_ascii(ref.nonce);
  const auto name_bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  UNICODE_STRING object_name{
      .Length = name_bytes, .MaximumLength = name_bytes, .Buffer = name.data()};
  OBJECT_ATTRIBUTES attributes{
      .Length = sizeof(OBJECT_ATTRIBUTES),
      .RootDirectory = nullptr,
      .ObjectName = &object_name,
      .Attributes = obj_case_insensitive | (permanent_ ? obj_permanent : 0U),
      .SecurityDescriptor = &descriptor,
      .SecurityQualityOfService = nullptr,
  };
  HANDLE raw = nullptr;
  if (const LONG status = create_job(&raw, job_access, &attributes); status < 0) {
    return nt_error("NtCreateJobObject(" + ref.object_name() + ")", status);
  }
  wil::unique_handle job{raw};

  Result<wil::unique_handle> port = attach_port(job.get());
  if (!port) {
    if (permanent_) {
      native().make_temporary(job.get());  // do not leave a permanent job behind
    }
    return tl::unexpected(port.error());
  }
  return std::make_unique<Win32Job>(std::move(ref), std::move(job), std::move(*port), permanent_);
}

Result<std::unique_ptr<os::IJob>> Win32JobControl::open(const os::JobRef& ref) {
  if (Result<void> valid = os::validate(ref); !valid) {
    return tl::unexpected(valid.error());
  }
  const std::string name = ref.object_name();
  wil::unique_handle job{::OpenJobObjectW(job_access, FALSE, widen_ascii(name).c_str())};
  if (!job) {
    return win32_error("OpenJobObjectW(" + name + ")", ::GetLastError());
  }

  // The owner is checked on the handle that will be used, so the object cannot be swapped.
  PSID owner = nullptr;
  wil::unique_hlocal descriptor;
  if (const DWORD error =
          ::GetSecurityInfo(job.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                            nullptr, nullptr, nullptr, wil::out_param(descriptor));
      error != ERROR_SUCCESS) {
    return win32_error("GetSecurityInfo(" + name + ")", error);
  }
  wil::unique_hlocal expected;
  if (::ConvertStringSidToSidW(widen_ascii(expected_owner_).c_str(), wil::out_param(expected)) ==
      FALSE) {
    return win32_error("ConvertStringSidToSidW(" + expected_owner_ + ")", ::GetLastError());
  }
  if (owner == nullptr || ::EqualSid(owner, expected.get()) == FALSE) {
    const Result<std::string> actual =
        owner == nullptr ? Result<std::string>("none") : sid_string(owner);
    return fail(Errc::refused, std::format("untrusted job {}: owner is {}, expected {}", name,
                                           actual ? *actual : "unknown", expected_owner_));
  }

  // A job keeps its first completion port for life (error 87 on a second one): a job created by
  // an earlier run has none for us.
  wil::unique_handle port;
  if (Result<wil::unique_handle> attached = attach_port(job.get()); attached) {
    port = std::move(*attached);
  } else if (attached.error().code != Errc::invalid_argument) {
    return tl::unexpected(attached.error());
  }

  const Result<bool> permanent = is_permanent(job.get(), name);
  if (!permanent) {
    return tl::unexpected(permanent.error());
  }
  return std::make_unique<Win32Job>(ref, std::move(job), std::move(port), *permanent);
}

Result<bool> Win32JobControl::in_any_job(const os::ProcessKey& process) const {
  Result<wil::unique_handle> handle = open_process(process, 0);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  BOOL member = FALSE;
  if (::IsProcessInJob(handle->get(), nullptr, &member) == FALSE) {
    return win32_error(std::format("IsProcessInJob(pid {})", process.pid), ::GetLastError());
  }
  return member != FALSE;
}

Result<std::string> current_user_sid() {
  Result<std::vector<std::byte>> user = token_user();
  if (!user) {
    return tl::unexpected(user.error());
  }
  return sid_string(user_sid_of(*user));
}

}  // namespace myclean::platform
