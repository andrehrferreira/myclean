// myclean.exe: the read-only command line and MCP server. See cli/cli.hpp.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "cli/cli.hpp"

namespace {

std::string utf8(const wchar_t* text) {
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) {
    return {};
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
  result.pop_back();  // the terminating NUL
  return result;
}

// An exception nothing caught ends the program with its message on stderr instead of a bare
// abort, so a failure on an unusual machine can be traced.
[[noreturn]] void report_and_abort() noexcept {
  try {
    if (const std::exception_ptr current = std::current_exception()) {
      std::rethrow_exception(current);
    }
    static_cast<void>(
        std::fputs("\nmyclean: internal error: terminate without an exception\n", stderr));
  } catch (const std::exception& error) {
    // C stdio: nothing here may throw again.
    static_cast<void>(std::fputs("\nmyclean: internal error: ", stderr));
    static_cast<void>(std::fputs(error.what(), stderr));
    static_cast<void>(std::fputs("\n", stderr));
  } catch (...) {
    static_cast<void>(std::fputs("\nmyclean: internal error: unknown exception\n", stderr));
  }
  static_cast<void>(std::fflush(stderr));
  std::abort();
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program entry point
int wmain(int argc, wchar_t** argv) {
  std::set_terminate(&report_and_abort);
  // Lines end in "\n" only: the MCP stdio transport and JSON consumers expect no "\r".
  static_cast<void>(::_setmode(::_fileno(stdout), _O_BINARY));
  static_cast<void>(::_setmode(::_fileno(stdin), _O_BINARY));
  const std::span<wchar_t*> raw(argv, static_cast<std::size_t>(argc));
  std::vector<std::string> args;
  for (const wchar_t* arg : raw.subspan(1)) {
    args.push_back(utf8(arg));
  }
  const myclean::os::SystemClock clock;
  return myclean::cli::run(args, std::cin, std::cout, std::cerr, clock);
}
