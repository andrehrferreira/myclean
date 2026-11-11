#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the other Win32 headers need its types
#include <AclAPI.h>
#include <sddl.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/placement/settings.hpp"
#include "core/security/verdicts.hpp"
#include "platform/packs/win32_packs.hpp"
#include "platform/security/win32_security.hpp"
#include "platform/security/wmi.hpp"
#include "platform/service/system.hpp"
#include "platform/settings/registry.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {
namespace {

using settings_detail::narrow;
using settings_detail::widen;
using settings_detail::win32_error;

constexpr std::wstring_view defender_space = LR"(ROOT\Microsoft\Windows\Defender)";
constexpr DWORD max_settings_file = DWORD{64} * 1024;

std::wstring environment(std::wstring_view variable) {
  std::array<wchar_t, MAX_PATH + 1> buffer{};
  const DWORD length = ::GetEnvironmentVariableW(std::wstring(variable).c_str(), buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  return {buffer.data(), length};
}

// MpCmdRun.exe of the current Defender platform, else the one of the Windows Defender folder.
Result<std::filesystem::path> mpcmdrun() {
  const Win32Registry machine;
  const Result<std::optional<placement::RegistryValue>> location =
      machine.read(R"(HKLM\SOFTWARE\Microsoft\Windows Defender)", "InstallLocation");
  std::vector<std::filesystem::path> candidates;
  const std::string folder =
      location ? location->value_or(placement::RegistryValue{}).text : std::string();
  if (!folder.empty()) {
    candidates.push_back(std::filesystem::path{widen(folder)} / L"MpCmdRun.exe");
  }
  const std::wstring program_files = environment(L"ProgramFiles");
  if (!program_files.empty()) {
    candidates.push_back(std::filesystem::path{program_files} /
                         LR"(Windows Defender\MpCmdRun.exe)");
  }
  for (const std::filesystem::path& candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
  }
  return fail(Errc::not_found, "MpCmdRun.exe was not found: Microsoft Defender is not installed");
}

}  // namespace

Result<std::vector<security::Detection>> Win32Defender::detections() const {
  const security_detail::ComScope com;
  if (Result<void> status = com.status(); !status) {
    return tl::unexpected(status.error());
  }
  Result<std::vector<security_detail::WmiRow>> threats =
      security_detail::wmi_query(defender_space, L"SELECT ThreatID, ThreatName FROM MSFT_MpThreat");
  if (!threats) {
    return tl::unexpected(threats.error());
  }
  Result<std::vector<security_detail::WmiRow>> rows = security_detail::wmi_query(
      defender_space,
      L"SELECT ThreatID, Resources, InitialDetectionTime FROM MSFT_MpThreatDetection");
  if (!rows) {
    return tl::unexpected(rows.error());
  }
  // ThreatID is a uint64, which WMI hands over as text.
  std::map<std::string, std::string> names;
  for (const security_detail::WmiRow& row : *threats) {
    names.emplace(security_detail::wmi_text(*row, L"ThreatID"),
                  security_detail::wmi_text(*row, L"ThreatName"));
  }
  std::vector<security::Detection> detections;
  detections.reserve(rows->size());
  for (const security_detail::WmiRow& row : *rows) {
    const std::string id = security_detail::wmi_text(*row, L"ThreatID");
    const auto name = names.find(id);
    detections.push_back(security::Detection{
        .threat = name != names.end() && !name->second.empty() ? name->second : "threat " + id,
        .resources = security_detail::wmi_texts(*row, L"Resources"),
        .time = security::parse_cim_datetime(
            security_detail::wmi_text(*row, L"InitialDetectionTime"))});
  }
  return detections;
}

Result<security::FileScan> Win32Defender::scan_file(const std::filesystem::path& file,
                                                    const std::stop_token& stop) {
  Result<std::filesystem::path> program = mpcmdrun();
  if (!program) {
    return tl::unexpected(program.error());
  }
  // A Windows path holds no double quote, so quoting it is enough.
  std::wstring line = L"\"" + program->native() + L"\" -Scan -ScanType 3 -File \"" + file.native() +
                      L"\" -DisableRemediation";
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
  if (::CreateProcessW(program->c_str(), line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup,
                       &process) == FALSE) {
    return win32_error("starting MpCmdRun.exe", ::GetLastError());
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
  // Waited in slices, so a service stop ends the scan within its budget.
  constexpr DWORD slice_ms = 250;
  DWORD waited = WAIT_TIMEOUT;
  for (auto left = timeout_; waited == WAIT_TIMEOUT && left.count() > 0 && !stop.stop_requested();
       left -= std::chrono::milliseconds{slice_ms}) {
    waited = ::WaitForSingleObject(process.hProcess, slice_ms);
  }
  if (waited != WAIT_OBJECT_0) {
    ::TerminateProcess(process.hProcess, 1);
    ::WaitForSingleObject(process.hProcess, INFINITE);
  }
  reader.join();
  if (waited != WAIT_OBJECT_0) {
    return fail(Errc::io_error, stop.stop_requested()
                                    ? "the Defender scan was stopped with the service"
                                    : "the Defender scan did not finish in time and was stopped");
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process.hProcess, &exit_code) == FALSE) {
    return win32_error("reading MpCmdRun's exit code", ::GetLastError());
  }
  return security::parse_file_scan(static_cast<int>(exit_code), narrow(widen(output, CP_OEMCP)));
}

Result<std::optional<std::string>> read_security_settings() {
  return read_owner_settings(L"security.toml");
}

Result<std::optional<std::string>> read_owner_settings(std::wstring_view name) {
  Result<std::filesystem::path> data = program_data_dir();
  if (!data) {
    return tl::unexpected(data.error());
  }
  const std::filesystem::path file = *data / L"MyClean" / name;
  std::error_code error;
  if (!std::filesystem::exists(file, error)) {
    return std::optional<std::string>{};
  }
  if (!std::filesystem::is_regular_file(file, error)) {
    return fail(Errc::invalid_data, utf8_text(file) + ": not a regular file");
  }
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (const DWORD status = ::GetNamedSecurityInfoW(
          file.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          nullptr, nullptr, nullptr, nullptr, &raw);
      status != ERROR_SUCCESS) {
    return win32_error("reading the permissions of " + utf8_text(file), status);
  }
  const wil::unique_hlocal_security_descriptor descriptor(raw);
  wil::unique_hlocal_string sddl;
  if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
          descriptor.get(), SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          wil::out_param(sddl), nullptr) == FALSE) {
    return win32_error("reading the permissions of " + utf8_text(file), ::GetLastError());
  }
  if (Result<void> checked = check_admin_write_only(sddl.get()); !checked) {
    return fail(Errc::refused, utf8_text(file) + " is ignored: " + checked.error().message);
  }
  const wil::unique_hfile handle{::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!handle) {
    return win32_error("opening " + utf8_text(file), ::GetLastError());
  }
  std::string text(std::size_t{max_settings_file} + 1, '\0');
  DWORD read = 0;
  if (::ReadFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) ==
      FALSE) {
    return win32_error("reading " + utf8_text(file), ::GetLastError());
  }
  if (read > max_settings_file) {
    return fail(Errc::invalid_data, utf8_text(file) + ": larger than 64 KiB");
  }
  text.resize(read);
  return std::optional<std::string>{std::move(text)};
}

}  // namespace myclean::platform
