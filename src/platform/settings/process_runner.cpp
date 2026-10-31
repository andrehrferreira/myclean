#include "platform/settings/process_runner.hpp"

#include <array>
#include <string>
#include <thread>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace {

using settings_detail::narrow;
using settings_detail::widen;
using settings_detail::win32_error;

// `argument` as CommandLineToArgvW reads it back: quoted when it holds a blank or is empty.
std::wstring quoted(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring result = L"\"";
  for (const wchar_t character : argument) {
    if (character == L'"') {
      result += L'\\';
    }
    result += character;
  }
  return result + L"\"";
}

}  // namespace

Result<placement::CommandResult> Win32ProcessRunner::run(const placement::Command& command) {
  const std::wstring tool = widen(command.tool);
  if (tool.empty() || tool.find_first_of(L"\\/:") != std::wstring::npos) {
    return fail(Errc::invalid_argument,
                "only a tool of the system folder may run: " + command.tool);
  }
  std::array<wchar_t, MAX_PATH> system{};
  const UINT length = ::GetSystemDirectoryW(system.data(), MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return win32_error("finding the system folder", ::GetLastError());
  }
  const std::wstring program = std::wstring(system.data(), length) + L"\\" + tool;
  std::wstring line = quoted(program);
  for (const std::string& argument : command.arguments) {
    line += L" " + quoted(widen(argument));
  }

  SECURITY_ATTRIBUTES inheritable{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                  .lpSecurityDescriptor = nullptr,
                                  .bInheritHandle = TRUE};
  wil::unique_handle read_end;
  wil::unique_handle write_end;
  if (::CreatePipe(read_end.put(), write_end.put(), &inheritable, 0) == FALSE ||
      ::SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
    return win32_error("creating the output pipe", ::GetLastError());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end.get();
  startup.hStdError = write_end.get();
  wil::unique_process_information process;
  if (::CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process) == FALSE) {
    return win32_error("starting " + command.text(), ::GetLastError());
  }
  write_end.reset();  // the child holds the only write end now, so its exit ends the reads

  std::string output;
  std::thread reader{[&] {
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (::ReadFile(read_end.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr) != FALSE &&
           read > 0) {
      output.append(buffer.data(), read);
    }
  }};
  const DWORD waited =
      ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_.count()));
  if (waited != WAIT_OBJECT_0) {
    ::TerminateProcess(process.hProcess, 1);
    ::WaitForSingleObject(process.hProcess, INFINITE);
  }
  reader.join();
  if (waited != WAIT_OBJECT_0) {
    return fail(Errc::io_error, command.text() + " did not finish in time and was stopped");
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process.hProcess, &exit_code) == FALSE) {
    return win32_error("reading the exit code of " + command.text(), ::GetLastError());
  }
  return placement::CommandResult{.exit_code = exit_code,
                                  .output = narrow(widen(output, CP_OEMCP))};
}

}  // namespace myclean::platform
