// myclean.exe: the read-only command line and MCP server. See cli/cli.hpp.

#include <cstddef>
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

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program entry point
int wmain(int argc, wchar_t** argv) {
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
