#include "platform/cleanup/win32_command_runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/cleanup/command_runner.hpp"
#include "core/model/result.hpp"
#include "platform/cleanup/wide.hpp"
#include "platform/ipc/security.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using cleanup::Command;
using cleanup::CommandOutput;

constexpr std::size_t output_limit = std::size_t{1} << 20U;  // per stream: the last 1 MiB is kept
constexpr std::size_t read_chunk = 4096;
constexpr DWORD poll_ms = 50;
constexpr std::array<std::wstring_view, 4> extensions = {L".com", L".exe", L".bat", L".cmd"};
constexpr std::wstring_view cmd_metacharacters = L"\"&|<>^%!()\r\n";
constexpr std::array<std::string_view, 3> service_sids = {"S-1-5-18", "S-1-5-19", "S-1-5-20"};

// UTF-8 of what a tool printed: kept when it is valid UTF-8 (node, go), else decoded from the
// OEM code page that console tools such as dism.exe use.
std::string decode(const std::string& bytes) {
  const int length = static_cast<int>(bytes.size());
  if (bytes.empty() ||
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0) > 0) {
    return bytes;
  }
  const int size = ::MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), length, nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), length, wide.data(), size);
  return narrow(wide);
}

// `argument` quoted so that CommandLineToArgvW and the C runtime read it back unchanged.
std::wstring quote(std::wstring_view argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    quoted.append(character == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::wstring lower(std::wstring text) {
  std::ranges::transform(text, text.begin(), [](wchar_t character) {
    return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a')
                                                  : character;
  });
  return text;
}

fs::path system_directory() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const UINT length = ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  return {std::wstring_view(buffer.data(), length)};
}

// Where every command starts: the profile folder of this process's user, never the folder this
// process happened to start in. Tools that pick a cache by the current drive (pnpm puts a store
// at the root of the drive when the profile is elsewhere) then act on the owner's real cache.
fs::path working_folder() {
  const DWORD size = ::GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
  std::wstring value(size, L'\0');
  value.resize(::GetEnvironmentVariableW(L"USERPROFILE", value.data(), size));
  const fs::path profile{value};
  std::error_code error;
  return profile.is_absolute() && fs::is_directory(profile, error) ? profile : system_directory();
}

std::vector<fs::path> path_folders() {
  const DWORD size = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
  std::wstring value(size, L'\0');
  value.resize(::GetEnvironmentVariableW(L"PATH", value.data(), size));
  std::vector<fs::path> folders;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = std::min(value.find(L';', start), value.size());
    std::wstring folder = value.substr(start, end - start);
    std::erase(folder, L'"');
    if (!folder.empty()) {
      folders.emplace_back(std::move(folder));
    }
    start = end + 1;
  }
  return folders;
}

Error win32_error(std::string_view what, DWORD code) {
  return {.code = code == ERROR_ACCESS_DENIED ? Errc::access_denied : Errc::io_error,
          .message = std::format("{} failed: {}", what,
                                 std::system_category().message(static_cast<int>(code)))};
}

// Reads `pipe` to its end into `text`, keeping the last output_limit bytes.
void drain(HANDLE pipe, std::string& text) {
  std::array<char, read_chunk> buffer{};
  DWORD read = 0;
  while (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) !=
             FALSE &&
         read > 0) {
    text.append(buffer.data(), read);
    if (text.size() > output_limit) {
      text.erase(0, text.size() - output_limit);
    }
  }
}

struct Pipe {
  wil::unique_hfile read;
  wil::unique_hfile write;  // inherited by the child
};

Result<Pipe> make_pipe(SECURITY_ATTRIBUTES& inherit) {
  Pipe pipe;
  if (::CreatePipe(pipe.read.put(), pipe.write.put(), &inherit, 0) == FALSE ||
      ::SetHandleInformation(pipe.read.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
    return tl::unexpected(win32_error("CreatePipe", ::GetLastError()));
  }
  return pipe;
}

// The application and command line that run `program` with `arguments`.
Result<std::pair<fs::path, std::wstring>> command_line(const fs::path& program,
                                                       const std::vector<std::string>& arguments) {
  const std::wstring extension = lower(program.extension().wstring());
  const bool script = extension == L".bat" || extension == L".cmd";
  std::wstring tail;
  for (const std::string& argument : arguments) {
    const std::wstring wide = widen(argument);
    if (script && wide.find_first_of(cmd_metacharacters) != std::wstring::npos) {
      return fail(Errc::invalid_argument,
                  "argument \"" + argument + "\" holds a cmd.exe metacharacter");
    }
    tail += L" " + quote(wide);
  }
  if (!script) {
    return std::pair{program, quote(program.wstring()) + tail};
  }
  const fs::path cmd = system_directory() / L"cmd.exe";
  return std::pair{
      cmd, quote(cmd.wstring()) + L" /d /s /c \"\"" + program.wstring() + L"\"" + tail + L"\""};
}

}  // namespace

Win32CommandRunner::Win32CommandRunner(std::vector<fs::path> search_path)
    : search_path_(std::move(search_path)) {}

bool Win32CommandRunner::user_context() const {
  const Result<std::string> sid = current_user_sid();
  return sid && std::ranges::find(service_sids, std::string_view{*sid}) == service_sids.end();
}

Result<fs::path> Win32CommandRunner::resolve(std::string_view program) const {
  fs::path name{widen(std::string(program))};
  std::error_code error;
  if (name.is_absolute()) {
    if (fs::is_regular_file(name, error)) {
      return name;
    }
    return fail(Errc::not_found, std::string(program) + " does not exist");
  }
  if (name.empty() || name.has_parent_path()) {
    return fail(Errc::invalid_argument,
                "a program is a bare name or an absolute path: " + std::string(program));
  }
  const bool has_extension =
      std::ranges::find(extensions, lower(name.extension().wstring())) != extensions.end();
  const std::vector<fs::path> folders = search_path_.empty() ? path_folders() : search_path_;
  for (const fs::path& folder : folders) {
    if (!folder.is_absolute()) {
      continue;
    }
    for (const std::wstring_view extension : extensions) {
      fs::path candidate = folder / name;
      if (!has_extension) {
        candidate += extension;
      }
      if (fs::is_regular_file(candidate, error)) {
        return candidate;
      }
      if (has_extension) {
        break;
      }
    }
  }
  return fail(Errc::not_found, std::string(program) + " was not found on PATH");
}

Result<CommandOutput> Win32CommandRunner::run(const Command& command, std::stop_token stop) {
  const Result<fs::path> program = resolve(command.program);
  if (!program) {
    return tl::unexpected(program.error());
  }
  Result<std::pair<fs::path, std::wstring>> line = command_line(*program, command.arguments);
  if (!line) {
    return tl::unexpected(line.error());
  }

  SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                              .lpSecurityDescriptor = nullptr,
                              .bInheritHandle = TRUE};
  Result<Pipe> out = make_pipe(inherit);
  Result<Pipe> err = make_pipe(inherit);
  if (!out || !err) {
    return tl::unexpected(out ? err.error() : out.error());
  }
  wil::unique_hfile input(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inherit, OPEN_EXISTING, 0, nullptr));
  if (!input) {
    return tl::unexpected(win32_error("CreateFile(NUL)", ::GetLastError()));
  }

  // Only these three handles are inherited.
  std::array<HANDLE, 3> inherited = {input.get(), out->write.get(), err->write.get()};
  SIZE_T list_size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  std::vector<std::byte> list_buffer(list_size);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an opaque Win32 buffer
  auto* const list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_buffer.data());
  if (::InitializeProcThreadAttributeList(list, 1, 0, &list_size) == FALSE) {
    return tl::unexpected(win32_error("InitializeProcThreadAttributeList", ::GetLastError()));
  }
  const auto delete_list = wil::scope_exit([&] { ::DeleteProcThreadAttributeList(list); });
  if (::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  static_cast<void*>(inherited.data()), sizeof(inherited), nullptr,
                                  nullptr) == FALSE) {
    return tl::unexpected(win32_error("UpdateProcThreadAttribute", ::GetLastError()));
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.StartupInfo.wShowWindow = SW_HIDE;
  startup.StartupInfo.hStdInput = input.get();
  startup.StartupInfo.hStdOutput = out->write.get();
  startup.StartupInfo.hStdError = err->write.get();
  startup.lpAttributeList = list;

  const wil::unique_handle job(::CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || ::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits)) == FALSE) {
    return tl::unexpected(win32_error("CreateJobObject", ::GetLastError()));
  }

  std::wstring& text = line->second;
  const fs::path folder = working_folder();
  wil::unique_process_information process;
  if (::CreateProcessW(line->first.c_str(), text.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       folder.c_str(), &startup.StartupInfo, &process) == FALSE) {
    return tl::unexpected(win32_error("CreateProcess " + command.program, ::GetLastError()));
  }
  if (::AssignProcessToJobObject(job.get(), process.hProcess) == FALSE) {
    const DWORD code = ::GetLastError();
    ::TerminateProcess(process.hProcess, 1);
    return tl::unexpected(win32_error("AssignProcessToJobObject", code));
  }
  ::ResumeThread(process.hThread);
  out->write.reset();
  err->write.reset();
  input.reset();

  std::string out_text;
  std::string err_text;
  std::thread out_reader([&] { drain(out->read.get(), out_text); });
  std::thread err_reader([&] { drain(err->read.get(), err_text); });

  const auto deadline = std::chrono::steady_clock::now() + command.timeout;
  bool timed_out = false;
  bool cancelled = false;
  while (::WaitForSingleObject(process.hProcess, poll_ms) == WAIT_TIMEOUT) {
    cancelled = stop.stop_requested();
    timed_out = std::chrono::steady_clock::now() >= deadline;
    if (cancelled || timed_out) {
      break;
    }
  }
  // Ends the tree on a timeout or cancel, and whatever the tool left behind holding the pipes.
  ::TerminateJobObject(job.get(), 1);
  ::WaitForSingleObject(process.hProcess, INFINITE);
  out_reader.join();
  err_reader.join();

  if (cancelled) {
    return fail(Errc::refused, "cancelled");
  }
  if (timed_out) {
    return fail(
        Errc::io_error,
        std::format("timed out after {} s",
                    std::chrono::duration_cast<std::chrono::seconds>(command.timeout).count()));
  }
  DWORD exit_code = 0;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  return CommandOutput{.exit_code = static_cast<std::uint32_t>(exit_code),
                       .out = decode(out_text),
                       .err = decode(err_text)};
}

fs::path dism_path() {
  return system_directory() / L"dism.exe";
}

}  // namespace myclean::platform
