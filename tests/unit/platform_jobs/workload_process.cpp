#include "workload_process.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/os/process_control.hpp"
#include "platform/jobs/win32_process_control.hpp"

namespace myclean::platform::testing {

namespace {

constexpr std::chrono::milliseconds teardown_wait{5000};
constexpr std::chrono::milliseconds probe_wait{15000};

std::filesystem::path workload_path() {
  return {MYCLEAN_JOBS_WORKLOAD};
}

std::chrono::nanoseconds nanoseconds_of(const FILETIME& time) {
  const std::uint64_t ticks = (std::uint64_t{time.dwHighDateTime} << 32U) | time.dwLowDateTime;
  return std::chrono::nanoseconds(ticks * 100U);
}

}  // namespace

Workload Workload::start(std::wstring_view arguments, bool suspended) {
  Workload workload;
  const std::filesystem::path path = workload_path();
  std::wstring command = L"\"" + path.wstring() + L"\" " + std::wstring(arguments);
  // No "app starting" cursor while it starts.
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW), .dwFlags = STARTF_FORCEOFFFEEDBACK};
  PROCESS_INFORMATION started{};
  const DWORD flags =
      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | (suspended ? CREATE_SUSPENDED : 0U);
  if (::CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                       nullptr, &startup, &started) == FALSE) {
    const DWORD error = ::GetLastError();
    std::error_code ignored;
    // Kaspersky refused to run, then deleted, a helper it disliked (spike X4 §3.8).
    const bool blocked = error == ERROR_VIRUS_INFECTED || error == ERROR_VIRUS_DELETED ||
                         error == ERROR_ACCESS_DENIED || !std::filesystem::exists(path, ignored);
    workload.failure_ = blocked ? Failure::blocked_by_antivirus : Failure::other;
    const std::u8string name = path.u8string();
    workload.error_ = std::format("CreateProcessW({}): Win32 error {}",
                                  std::string(name.begin(), name.end()), error);
    return workload;
  }
  workload.process_.reset(started.hProcess);
  workload.thread_.reset(started.hThread);
  const Result<os::ProcessKey> key = process_key(started.dwProcessId);
  if (!key) {
    workload.failure_ = Failure::other;
    workload.error_ = key.error().message;
    return workload;
  }
  workload.key_ = *key;
  return workload;
}

Workload::~Workload() {
  if (process_) {
    ::TerminateProcess(process_.get(), 1);
    ::WaitForSingleObject(process_.get(), static_cast<DWORD>(teardown_wait.count()));
  }
}

void Workload::resume() const {
  ::ResumeThread(thread_.get());
}

std::optional<std::uint32_t> Workload::wait(std::chrono::milliseconds timeout) const {
  if (::WaitForSingleObject(process_.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
    return std::nullopt;
  }
  DWORD code = 0;
  ::GetExitCodeProcess(process_.get(), &code);
  return code;
}

std::chrono::nanoseconds Workload::cpu_time() const {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  ::GetProcessTimes(process_.get(), &created, &exited, &kernel, &user);
  return nanoseconds_of(kernel) + nanoseconds_of(user);
}

std::optional<std::string> antivirus_blocks_workload() {
  const Workload probe = Workload::start(L"exit 0 0");
  if (!probe.started()) {
    if (probe.failure() == Workload::Failure::blocked_by_antivirus) {
      return "blocked by AV: " + probe.error();
    }
    return std::nullopt;  // another failure: let the test report it
  }
  const std::optional<std::uint32_t> code = probe.wait(probe_wait);
  if (code != 0U) {
    return std::format("blocked by AV: the workload probe {}",
                       code ? std::format("exited with {:#x}", *code) : "did not finish");
  }
  return std::nullopt;
}

}  // namespace myclean::platform::testing
