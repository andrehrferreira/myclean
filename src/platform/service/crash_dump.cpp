#include "platform/service/crash_dump.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#include <wil/resource.h>
// clang-format off
#include <windows.h>  // first: dbghelp.h needs its types
#include <dbghelp.h>
// clang-format on

#include "core/model/result.hpp"
#include "platform/ipc/pipe_io.hpp"
#include "platform/service/system.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

fs::path& dump_dir() {
  static fs::path dir;
  return dir;
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* exception) {
  static_cast<void>(write_crash_dump(dump_dir(), exception));
  return EXCEPTION_CONTINUE_SEARCH;  // the default handling (WER) still ends the process
}

[[noreturn]] void on_terminate() {
  static_cast<void>(write_crash_dump(dump_dir(), nullptr));
  std::abort();
}

}  // namespace

Result<fs::path> write_crash_dump(const fs::path& dir, EXCEPTION_POINTERS* exception) {
  const Result<fs::path> exe = executable_path();
  SYSTEMTIME now{};
  ::GetSystemTime(&now);
  fs::path path = dir / std::format(L"{}-{}-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp",
                                    exe ? exe->stem().wstring() : std::wstring(L"process"),
                                    ::GetCurrentProcessId(), now.wYear, now.wMonth, now.wDay,
                                    now.wHour, now.wMinute, now.wSecond);
  const wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                             FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) {
    return pipe_io::failure("CreateFile " + pipe_io::utf8(path.native()), ::GetLastError());
  }
  MINIDUMP_EXCEPTION_INFORMATION information{
      .ThreadId = ::GetCurrentThreadId(), .ExceptionPointers = exception, .ClientPointers = FALSE};
  // MINIDUMP_TYPE is a set of flags, so a combination is no enumerator.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto type =
      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                 MiniDumpWithUnloadedModules);
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  if (::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file.get(), type,
                          exception != nullptr ? &information : nullptr, nullptr,
                          nullptr) == FALSE) {
    return pipe_io::failure("MiniDumpWriteDump", ::GetLastError());
  }
  return path;
}

Result<void> prune_crash_dumps(const fs::path& dir, std::size_t keep) {
  try {
    std::vector<fs::directory_entry> dumps;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.path().extension() == L".dmp") {
        dumps.push_back(entry);
      }
    }
    std::ranges::sort(dumps, [](const fs::directory_entry& left, const fs::directory_entry& right) {
      return left.last_write_time() > right.last_write_time();
    });
    for (std::size_t index = keep; index < dumps.size(); ++index) {
      fs::remove(dumps[index].path());
    }
    return {};
  } catch (const fs::filesystem_error& error) {
    return fail(Errc::io_error, std::string("pruning crash dumps: ") + error.what());
  }
}

Result<void> install_crash_dumps(const fs::path& dir, std::size_t keep) {
  std::error_code error;
  fs::create_directories(dir, error);
  if (error) {
    return fail(Errc::io_error, "creating " + pipe_io::utf8(dir.native()) + ": " + error.message());
  }
  if (Result<void> pruned = prune_crash_dumps(dir, keep == 0 ? 0 : keep - 1); !pruned) {
    return pruned;
  }
  dump_dir() = dir;
  ::SetUnhandledExceptionFilter(&on_unhandled_exception);
  std::set_terminate(&on_terminate);
  return {};
}

}  // namespace myclean::platform
