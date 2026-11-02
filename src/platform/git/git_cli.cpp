#include "platform/git/git_cli.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

tl::unexpected<Error> last_error(const std::string& action) {
  return fail(Errc::io_error, action + ": Win32 error " + std::to_string(::GetLastError()));
}

// `argument` quoted for CommandLineToArgvW and the C runtime's argument parser.
std::wstring quoted(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    // Backslashes are literal unless a quote follows them.
    result.append(character == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
    result.push_back(character);
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');  // they precede the closing quote
  result.push_back(L'"');
  return result;
}

// `entry` relative to `work_tree`, with '/' separators, as git takes paths.
Result<std::wstring> git_path(const fs::path& work_tree, const fs::path& entry) {
  const fs::path relative = entry.lexically_relative(work_tree);
  if (relative.empty() || *relative.begin() == L"..") {
    return fail(Errc::invalid_argument, utf8(entry) + " is not inside " + utf8(work_tree));
  }
  return relative.generic_wstring();
}

tl::unexpected<Error> git_failed(const fs::path& work_tree, const char* command,
                                 std::uint32_t exit_code) {
  return fail(Errc::io_error, std::string("git ") + command + " failed in " + utf8(work_tree) +
                                  " with exit code " + std::to_string(exit_code));
}

}  // namespace

Result<ProcessOutput> run_hidden(const fs::path& program, std::span<const std::wstring> arguments,
                                 std::chrono::milliseconds timeout,
                                 const ProcessIdentity* identity) {
  SECURITY_ATTRIBUTES inheritable{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                  .lpSecurityDescriptor = nullptr,
                                  .bInheritHandle = TRUE};
  wil::unique_hfile read_end;
  wil::unique_hfile write_end;
  if (::CreatePipe(read_end.put(), write_end.put(), &inheritable, 0) == FALSE ||
      ::SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
    return last_error("cannot create a pipe");
  }
  wil::unique_hfile null_device{::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                              OPEN_EXISTING, 0, nullptr)};
  if (!null_device) {
    return last_error("cannot open NUL");
  }

  // Only the two standard handles are inherited, whatever else this process has open.
  SIZE_T attributes_size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
  std::vector<std::byte> attributes_buffer(attributes_size);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an opaque, caller-sized buffer
  auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
  if (::InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size) == FALSE) {
    return last_error("cannot prepare process attributes");
  }
  const auto delete_attributes =
      wil::scope_exit([attributes] { ::DeleteProcThreadAttributeList(attributes); });
  std::array<HANDLE, 2> inherited = {write_end.get(), null_device.get()};
  if (::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  static_cast<void*>(inherited.data()), sizeof(inherited), nullptr,
                                  nullptr) == FALSE) {
    return last_error("cannot prepare process attributes");
  }

  const wil::unique_handle job{::CreateJobObjectW(nullptr, nullptr)};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || ::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits)) == FALSE) {
    return last_error("cannot create a job");
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = null_device.get();
  startup.StartupInfo.hStdOutput = write_end.get();
  startup.StartupInfo.hStdError = null_device.get();
  startup.lpAttributeList = attributes;
  std::wstring command_line = quoted(program.native());
  for (const std::wstring& argument : arguments) {
    command_line += L' ' + quoted(argument);
  }
  wil::unique_process_information process;
  constexpr DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
  if (identity == nullptr) {
    if (::CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                         nullptr, nullptr, &startup.StartupInfo, &process) == FALSE) {
      return last_error("cannot start " + utf8(program));
    }
  } else {
    // The user's own desktop: a service's desktop is closed to the user's token.
    std::wstring desktop = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop.data();
    std::wstring environment = identity->environment;
    if (::CreateProcessAsUserW(identity->token.get(), program.c_str(), command_line.data(), nullptr,
                               nullptr, TRUE, flags | CREATE_UNICODE_ENVIRONMENT,
                               environment.data(), nullptr, &startup.StartupInfo,
                               &process) == FALSE) {
      return last_error("cannot start " + utf8(program) + " as the owner");
    }
  }
  if (::AssignProcessToJobObject(job.get(), process.hProcess) == FALSE) {
    ::TerminateProcess(process.hProcess, 1);
    return last_error("cannot start " + utf8(program) + " in a job");
  }
  ::ResumeThread(process.hThread);
  write_end.reset();  // the pipe ends when every process of the job has exited
  null_device.reset();

  ProcessOutput output;
  std::thread reader([&read_end, &output] {
    std::array<char, 4096> chunk{};
    DWORD read = 0;
    while (::ReadFile(read_end.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read,
                      nullptr) != FALSE &&
           read > 0) {
      output.out.append(chunk.data(), read);
    }
  });
  const DWORD waited = ::WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count()));
  ::TerminateJobObject(job.get(), 1);  // also ends anything the program left running
  reader.join();
  if (waited != WAIT_OBJECT_0) {
    return fail(Errc::io_error, utf8(program) + " did not finish within " +
                                    std::to_string(timeout.count()) + " ms");
  }
  DWORD exit_code = 0;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  output.exit_code = exit_code;
  return output;
}

std::optional<fs::path> find_git() {
  std::wstring path(::GetEnvironmentVariableW(L"PATH", nullptr, 0), L'\0');
  path.resize(::GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
  for (std::size_t start = 0; start < path.size();) {
    std::size_t end = path.find(L';', start);
    end = end == std::wstring::npos ? path.size() : end;
    std::wstring folder = path.substr(start, end - start);
    std::erase(folder, L'"');
    start = end + 1;
    if (folder.empty()) {
      continue;
    }
    fs::path candidate = fs::path{folder} / L"git.exe";
    const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

GitCli::GitCli(fs::path git, IdentitySource identity, std::chrono::milliseconds timeout)
    : git_(std::move(git)), identity_(std::move(identity)), timeout_(timeout) {}

Result<ProcessOutput> GitCli::run(const fs::path& work_tree,
                                  std::vector<std::wstring> arguments) const {
  std::vector<std::wstring> full = {L"--no-optional-locks", L"-c", L"core.fsmonitor=false", L"-C",
                                    work_tree.native()};
  full.insert(full.end(), std::make_move_iterator(arguments.begin()),
              std::make_move_iterator(arguments.end()));
  if (!identity_) {
    return run_hidden(git_, full, timeout_);
  }
  const Result<ProcessIdentity> identity = identity_();
  if (!identity) {
    return tl::unexpected(identity.error());
  }
  return run_hidden(git_, full, timeout_, &*identity);
}

Result<std::optional<WallTime>> GitCli::last_commit(const fs::path& work_tree) const {
  const Result<ProcessOutput> head = run(work_tree, {L"rev-parse", L"-q", L"--verify", L"HEAD"});
  if (!head) {
    return tl::unexpected(head.error());
  }
  if (head->exit_code == 1) {
    return std::nullopt;  // no commit yet
  }
  if (head->exit_code != 0) {
    return git_failed(work_tree, "rev-parse", head->exit_code);
  }
  const Result<ProcessOutput> log = run(work_tree, {L"log", L"-1", L"--format=%ct", L"HEAD"});
  if (!log) {
    return tl::unexpected(log.error());
  }
  std::int64_t seconds = 0;
  const std::string& text = log->out;
  if (log->exit_code != 0 ||
      std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), seconds).ec !=
          std::errc{}) {
    return git_failed(work_tree, "log", log->exit_code);
  }
  return WallTime{std::chrono::seconds{seconds}};
}

Result<bool> GitCli::ignored(const fs::path& work_tree, const fs::path& entry) const {
  const Result<std::wstring> path = git_path(work_tree, entry);
  if (!path) {
    return tl::unexpected(path.error());
  }
  const Result<ProcessOutput> checked = run(work_tree, {L"check-ignore", L"-q", L"--", *path});
  if (!checked) {
    return tl::unexpected(checked.error());
  }
  if (checked->exit_code > 1) {
    return git_failed(work_tree, "check-ignore", checked->exit_code);
  }
  return checked->exit_code == 0;
}

Result<bool> GitCli::tracks_files(const fs::path& work_tree, const fs::path& entry) const {
  const Result<std::wstring> path = git_path(work_tree, entry);
  if (!path) {
    return tl::unexpected(path.error());
  }
  const Result<ProcessOutput> listed =
      run(work_tree, {L"ls-files", L"-z", L"--", L":(literal)" + *path});
  if (!listed) {
    return tl::unexpected(listed.error());
  }
  if (listed->exit_code != 0) {
    return git_failed(work_tree, "ls-files", listed->exit_code);
  }
  return !listed->out.empty();
}

}  // namespace myclean::platform
