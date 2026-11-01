// Dummy workloads for the platform_jobs tests: the only processes those tests act on. Every mode
// ends by itself after the time it is given, shows nothing and reads no input. Built for the
// Windows subsystem, so no console host process is created next to it.
//
//   workload exit  <ms> <code>              sleep, then exit with <code> (decimal or 0x hex)
//   workload spin  <threads> <ms>           <threads> busy threads for <ms>, then exit 0
//   workload commit <chunk MiB> <total MiB> <hold ms>
//                                           commit <total> in chunks with VirtualAlloc, or until a
//                                           commit fails, and hold what it got; exit 0, or 3 when
//                                           a commit failed with ERROR_COMMITMENT_LIMIT, 4 on any
//                                           other failure
//   workload child <ms> <code>              start `workload exit <ms> <code>`, wait for it,
//                                           exit 0 (5 when the child cannot start)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crtdbg.h>
#include <windows.h>

namespace {

constexpr int exit_usage = 2;
constexpr int exit_commit_limit = 3;
constexpr int exit_commit_other = 4;
constexpr int exit_child_failed = 5;
constexpr std::size_t mib = std::size_t{1} << 20U;

std::uint32_t number(const wchar_t* text) {
  return static_cast<std::uint32_t>(std::wcstoul(text, nullptr, 0));
}

void pause_ms(std::uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int spin(std::uint32_t threads, std::uint32_t ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  std::atomic<std::uint64_t> sink{0};
  std::vector<std::jthread> workers;
  workers.reserve(threads);
  for (std::uint32_t i = 0; i < threads; ++i) {
    workers.emplace_back([&] {
      std::uint64_t local = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        for (int j = 0; j < 100'000; ++j) {
          local += static_cast<std::uint64_t>(j);
        }
      }
      sink += local;
    });
  }
  return 0;
}

int commit(std::uint32_t chunk_mib, std::uint32_t total_mib, std::uint32_t hold_ms) {
  std::vector<void*> chunks;
  int result = 0;
  for (std::uint32_t done = 0; done < total_mib; done += chunk_mib) {
    void* chunk =
        ::VirtualAlloc(nullptr, chunk_mib * mib, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (chunk == nullptr) {
      result = ::GetLastError() == ERROR_COMMITMENT_LIMIT ? exit_commit_limit : exit_commit_other;
      break;
    }
    chunks.push_back(chunk);
  }
  pause_ms(hold_ms);
  for (void* chunk : chunks) {
    ::VirtualFree(chunk, 0, MEM_RELEASE);
  }
  return result;
}

int child(std::uint32_t ms, const wchar_t* code) {
  std::wstring self(MAX_PATH, L'\0');
  self.resize(::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
  std::wstring command =
      L"\"" + self + L"\" exit " + std::to_wstring(ms) + L" " + std::wstring(code);
  // No "app starting" cursor while the child starts.
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW), .dwFlags = STARTF_FORCEOFFFEEDBACK};
  PROCESS_INFORMATION started{};
  if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &started) == FALSE) {
    return exit_child_failed;
  }
  ::WaitForSingleObject(started.hProcess, INFINITE);
  ::CloseHandle(started.hThread);
  ::CloseHandle(started.hProcess);
  return 0;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program's entry point
int wmain(int argc, wchar_t** argv) {
  // No dialog of any kind: no error boxes, no CRT report windows.
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

  const std::span<wchar_t*> args(argv, static_cast<std::size_t>(argc));
  const std::wstring_view mode = args.size() > 1 ? args[1] : L"";
  if (args.size() == 4 && mode == L"exit") {
    pause_ms(number(args[2]));
    return static_cast<int>(number(args[3]));
  }
  if (args.size() == 4 && mode == L"spin") {
    return spin(number(args[2]), number(args[3]));
  }
  if (args.size() == 5 && mode == L"commit") {
    return commit(number(args[2]), number(args[3]), number(args[4]));
  }
  if (args.size() == 4 && mode == L"child") {
    return child(number(args[2]), args[3]);
  }
  return exit_usage;
}
