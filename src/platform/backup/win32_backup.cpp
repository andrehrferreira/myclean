#include "platform/backup/win32_backup.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the COM and WMI headers need its types
#include <oleauto.h>
#include <wbemidl.h>
// clang-format on

#include "core/backup/backup.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/git/git_cli.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using Row = wil::com_ptr_nothrow<IWbemClassObject>;

constexpr long query_timeout_ms = 60'000;
constexpr std::chrono::minutes wbadmin_timeout{2};
constexpr std::wstring_view uninstall_path =
    LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::wstring_view uninstall_path32 =
    LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::wstring_view shell_folders =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)";
constexpr std::wstring_view onedrive_accounts = LR"(Software\Microsoft\OneDrive\Accounts)";
// wbadmin's exit code when the caller lacks the rights (spike X8).
constexpr DWORD wbadmin_access_denied = static_cast<DWORD>(-2);

std::wstring widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
  return result;
}

std::string narrow(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

std::wstring lower(std::wstring text) {
  std::ranges::transform(text, text.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
  });
  return text;
}

tl::unexpected<Error> win32_failed(std::string_view what, LSTATUS status) {
  Errc code = Errc::io_error;
  if (status == ERROR_ACCESS_DENIED) {
    code = Errc::access_denied;
  } else if (status == ERROR_FILE_NOT_FOUND) {
    code = Errc::not_found;
  }
  return fail(code, std::format("{}: Win32 error {}", what, status));
}

WallTime wall_time_of(const FILETIME& time) {
  // FILETIME counts 100 ns since 1601-01-01; the Unix epoch is 11644473600 s later.
  constexpr std::int64_t epoch_difference = 116'444'736'000'000'000;
  const std::int64_t ticks =
      (static_cast<std::int64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  return WallTime{std::chrono::duration_cast<WallTime::duration>(
      std::chrono::nanoseconds{(ticks - epoch_difference) * 100})};
}

// A string value (REG_SZ or REG_EXPAND_SZ, unexpanded); nullopt when the key or value is missing
// or of another type.
std::optional<std::wstring> reg_string(HKEY root, const std::wstring& key, const wchar_t* name) {
  DWORD size = 0;
  constexpr DWORD flags =
      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY;
  if (::RegGetValueW(root, key.c_str(), name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t)) {
    return std::nullopt;
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(root, key.c_str(), name, flags, nullptr, value.data(), &size) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  value.resize(std::wcslen(value.c_str()));
  return value;
}

std::optional<std::uint64_t> reg_qword(HKEY root, const std::wstring& key, const wchar_t* name) {
  std::uint64_t value = 0;
  DWORD size = sizeof(value);
  if (::RegGetValueW(root, key.c_str(), name, RRF_RT_QWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

// The subkey names of `key`; empty when it does not exist.
Result<std::vector<std::wstring>> subkeys(HKEY root, const std::wstring& key) {
  wil::unique_hkey handle;
  const LSTATUS opened =
      ::RegOpenKeyExW(root, key.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, handle.put());
  if (opened == ERROR_FILE_NOT_FOUND) {
    return std::vector<std::wstring>{};
  }
  if (opened != ERROR_SUCCESS) {
    return win32_failed("opening " + narrow(key), opened);
  }
  std::vector<std::wstring> names;
  constexpr DWORD max_key_name = 256;
  for (DWORD index = 0;; ++index) {
    std::array<wchar_t, max_key_name> name{};
    DWORD length = max_key_name;
    const LSTATUS status = ::RegEnumKeyExW(handle.get(), index, name.data(), &length, nullptr,
                                           nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) {
      return names;
    }
    if (status != ERROR_SUCCESS) {
      return win32_failed("listing " + narrow(key), status);
    }
    names.emplace_back(name.data(), length);
  }
}

std::wstring expand(const std::wstring& text) {
  const DWORD length = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
  if (length == 0) {
    return text;
  }
  std::wstring result(length, L'\0');
  ::ExpandEnvironmentStringsW(text.c_str(), result.data(), length);
  result.resize(std::wcslen(result.c_str()));
  return result;
}

// `text` with %USERPROFILE% (any case) replaced by `profile`, then the machine's variables.
std::wstring expand_for(std::wstring text, const std::wstring& profile) {
  constexpr std::wstring_view variable = L"%userprofile%";
  for (std::size_t at = lower(text).find(variable); at != std::wstring::npos;
       at = lower(text).find(variable)) {
    text.replace(at, variable.size(), profile);
  }
  return expand(text);
}

bool under(const fs::path& path, const fs::path& folder) {
  std::wstring inner = lower(path.lexically_normal().native());
  std::wstring outer = lower(folder.lexically_normal().native());
  while (!outer.empty() && outer.back() == L'\\') {
    outer.pop_back();
  }
  return !outer.empty() && inner.starts_with(outer) &&
         (inner.size() == outer.size() || inner[outer.size()] == L'\\');
}

tl::unexpected<Error> wmi_error(std::string_view what, HRESULT result) {
  const auto code = static_cast<std::uint32_t>(result);
  if (result == WBEM_E_ACCESS_DENIED || result == E_ACCESSDENIED) {
    return fail(Errc::access_denied,
                std::format("{} needs administrator rights (WMI error 0x{:08x})", what, code));
  }
  return fail(Errc::io_error, std::format("{} failed (WMI error 0x{:08x})", what, code));
}

// COM for the calling thread, for the lifetime of the scope.
class ComScope {
 public:
  ComScope() : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;
  ~ComScope() {
    if (SUCCEEDED(result_)) {
      ::CoUninitialize();
    }
  }

  [[nodiscard]] Result<void> status() const {
    if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
      return wmi_error("COM initialization", result_);
    }
    return {};
  }

 private:
  HRESULT result_;
};

// The rows of `wql` in WMI namespace `name_space`. Call inside a ComScope.
Result<std::vector<Row>> wmi_query(const wchar_t* name_space, const wchar_t* wql) {
  wil::com_ptr_nothrow<IWbemLocator> locator;
  if (const HRESULT result = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(locator.put()));
      FAILED(result)) {
    return wmi_error("creating the WMI locator", result);
  }
  const wil::unique_bstr path{::SysAllocString(name_space)};
  wil::com_ptr_nothrow<IWbemServices> services;
  if (const HRESULT result = locator->ConnectServer(path.get(), nullptr, nullptr, nullptr, 0,
                                                    nullptr, nullptr, services.put());
      FAILED(result)) {
    return wmi_error("connecting to WMI " + narrow(name_space), result);
  }
  if (const HRESULT result = ::CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT,
                                                 RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
      FAILED(result)) {
    return wmi_error("setting the WMI proxy security", result);
  }
  const wil::unique_bstr language{::SysAllocString(L"WQL")};
  const wil::unique_bstr text{::SysAllocString(wql)};
  wil::com_ptr_nothrow<IEnumWbemClassObject> rows;
  if (const HRESULT result = services->ExecQuery(
          language.get(), text.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
          nullptr, rows.put());
      FAILED(result)) {
    return wmi_error(narrow(wql), result);
  }
  std::vector<Row> result;
  for (;;) {
    Row row;
    ULONG count = 0;
    const HRESULT next = rows->Next(query_timeout_ms, 1, row.put(), &count);
    if (FAILED(next) || next == WBEM_S_TIMEDOUT) {
      return wmi_error(narrow(wql), next);
    }
    if (count == 0) {
      return result;
    }
    result.push_back(std::move(row));
  }
}

std::optional<std::wstring> text_of(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr)) ||
      value.vt != VT_BSTR) {  // NOLINT(cppcoreguidelines-pro-type-union-access): the tag
    return std::nullopt;
  }
  return std::wstring{value.bstrVal};  // NOLINT(cppcoreguidelines-pro-type-union-access): VT_BSTR
}

// The owner's profile folder (ProfileList\<sid>\ProfileImagePath, expanded).
Result<fs::path> profile_of(const std::string& sid) {
  const std::optional<std::wstring> path =
      reg_string(HKEY_LOCAL_MACHINE,
                 LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\)" + widen(sid),
                 L"ProfileImagePath");
  if (!path) {
    return fail(Errc::not_found, "no profile folder is registered for " + sid);
  }
  return fs::path{expand(*path)};
}

}  // namespace

std::optional<WallTime> cim_time(std::wstring_view text) {
  // yyyymmddHHMMSS.mmmmmmsUUU
  constexpr std::size_t length = 25;
  if (text.size() < length || text[14] != L'.' || (text[21] != L'+' && text[21] != L'-')) {
    return std::nullopt;
  }
  const auto number = [&](std::size_t at, std::size_t count) -> std::optional<int> {
    int value = 0;
    for (std::size_t index = at; index < at + count; ++index) {
      if (text[index] < L'0' || text[index] > L'9') {
        return std::nullopt;
      }
      value = value * 10 + (text[index] - L'0');
    }
    return value;
  };
  const auto year = number(0, 4);
  const auto month = number(4, 2);
  const auto day = number(6, 2);
  const auto hour = number(8, 2);
  const auto minute = number(10, 2);
  const auto second = number(12, 2);
  const auto offset = number(22, 3);
  if (!year || !month || !day || !hour || !minute || !second || !offset) {
    return std::nullopt;
  }
  const std::chrono::year_month_day date{std::chrono::year{*year},
                                         std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  const std::chrono::minutes utc_offset{text[21] == L'-' ? -*offset : *offset};
  return WallTime{std::chrono::sys_days{date}} + std::chrono::hours{*hour} +
         std::chrono::minutes{*minute} + std::chrono::seconds{*second} - utc_offset;
}

Result<backup::FileHistoryFacts> Win32BackupSystem::file_history() const {
  Result<fs::path> profile = profile_of(owner_sid_);
  if (!profile) {
    return tl::unexpected(profile.error());
  }
  const std::wstring hive = widen(owner_sid_) + L"\\" + std::wstring(shell_folders);
  const std::optional<std::wstring> local = reg_string(HKEY_USERS, hive, L"Local AppData");
  const fs::path local_app_data =
      local ? fs::path{expand_for(*local, profile->native())} : *profile / L"AppData" / L"Local";
  const fs::path folder =
      local_app_data / L"Microsoft" / L"Windows" / L"FileHistory" / L"Configuration";
  const fs::path config = folder / L"Config1.xml";
  std::error_code error;
  if (!fs::is_regular_file(config, error)) {
    return backup::FileHistoryFacts{};
  }
  backup::FileHistoryFacts facts{.configured = true};
  // The target: the text of <TargetUrl> (the file is UTF-16 with a BOM, or UTF-8).
  const wil::unique_hfile file{::CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file) {
    constexpr DWORD max_config = 1U << 20U;
    std::string bytes(max_config, '\0');
    DWORD read = 0;
    if (::ReadFile(file.get(), bytes.data(), max_config, &read, nullptr) != FALSE) {
      bytes.resize(read);
      std::wstring text;
      if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
          static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text.resize((bytes.size() - 2) / sizeof(wchar_t));
        std::memcpy(text.data(), bytes.substr(2).data(), text.size() * sizeof(wchar_t));
      } else {
        text = widen(bytes);
      }
      constexpr std::wstring_view open = L"<TargetUrl>";
      const std::size_t start = text.find(open);
      const std::size_t end = text.find(L"</TargetUrl>");
      if (start != std::wstring::npos && end != std::wstring::npos && end > start) {
        facts.target =
            narrow(std::wstring_view(text).substr(start + open.size(), end - start - open.size()));
      }
    }
  }
  for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &data) != FALSE &&
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      const WallTime written = wall_time_of(data.ftLastWriteTime);
      facts.newest = std::max(facts.newest.value_or(written), written);
    }
  }
  return facts;
}

Result<bool> Win32BackupSystem::image_versions() const {
  std::array<wchar_t, MAX_PATH> system{};
  const UINT length = ::GetSystemDirectoryW(system.data(), MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return fail(Errc::io_error, "the system folder could not be read");
  }
  const std::array<std::wstring, 2> arguments = {L"get", L"versions"};
  Result<ProcessOutput> output =
      run_hidden(fs::path{std::wstring_view(system.data(), length)} / L"wbadmin.exe", arguments,
                 wbadmin_timeout);
  if (!output) {
    return tl::unexpected(output.error());
  }
  if (output->exit_code == wbadmin_access_denied) {
    return fail(Errc::access_denied, "wbadmin get versions needs administrator rights");
  }
  return output->exit_code == 0;
}

Result<std::vector<backup::RestorePoint>> Win32BackupSystem::restore_points() const {
  const ComScope com;
  if (Result<void> status = com.status(); !status) {
    return tl::unexpected(status.error());
  }
  Result<std::vector<Row>> rows =
      wmi_query(LR"(ROOT\DEFAULT)", L"SELECT CreationTime, Description FROM SystemRestore");
  if (!rows) {
    return tl::unexpected(rows.error());
  }
  std::vector<backup::RestorePoint> points;
  for (const Row& row : *rows) {
    const std::optional<std::wstring> created = text_of(*row, L"CreationTime");
    const std::optional<WallTime> time = created ? cim_time(*created) : std::nullopt;
    if (time) {
      points.push_back(backup::RestorePoint{
          .created = *time, .description = narrow(text_of(*row, L"Description").value_or(L""))});
    }
  }
  return points;
}

Result<std::vector<backup::ShadowCopy>> Win32BackupSystem::shadow_copies() const {
  const ComScope com;
  if (Result<void> status = com.status(); !status) {
    return tl::unexpected(status.error());
  }
  Result<std::vector<Row>> rows =
      wmi_query(LR"(ROOT\CIMV2)", L"SELECT VolumeName, InstallDate FROM Win32_ShadowCopy");
  if (!rows) {
    return tl::unexpected(rows.error());
  }
  std::vector<backup::ShadowCopy> copies;
  for (const Row& row : *rows) {
    const std::optional<std::wstring> installed = text_of(*row, L"InstallDate");
    const std::optional<WallTime> time = installed ? cim_time(*installed) : std::nullopt;
    if (time) {
      copies.push_back(backup::ShadowCopy{
          .volume = narrow(text_of(*row, L"VolumeName").value_or(L"")), .created = *time});
    }
  }
  return copies;
}

Result<std::vector<backup::OneDriveAccount>> Win32BackupSystem::onedrive() const {
  Result<fs::path> profile = profile_of(owner_sid_);
  if (!profile) {
    return tl::unexpected(profile.error());
  }
  const std::wstring user = widen(owner_sid_) + L"\\";
  Result<std::vector<std::wstring>> names =
      subkeys(HKEY_USERS, user + std::wstring(onedrive_accounts));
  if (!names) {
    return tl::unexpected(names.error());
  }
  const std::array<std::pair<const wchar_t*, std::string_view>, 3> known = {
      std::pair{L"Desktop", std::string_view("Desktop")},
      std::pair{L"Personal", std::string_view("Documents")},
      std::pair{L"My Pictures", std::string_view("Pictures")}};
  std::vector<backup::OneDriveAccount> accounts;
  for (const std::wstring& name : *names) {
    std::wstring key = user;
    key.append(onedrive_accounts).append(L"\\").append(name);
    const std::optional<std::wstring> folder = reg_string(HKEY_USERS, key, L"UserFolder");
    if (!folder || folder->empty()) {
      continue;  // FileCoAuth and accounts never signed in have no folder
    }
    backup::OneDriveAccount account{.account = narrow(name), .folder = fs::path{*folder}};
    if (const std::optional<std::uint64_t> since =
            reg_qword(HKEY_USERS, key, L"SignInErrorStateStartTimestamp");
        since && *since > 0) {
      account.sign_in_error = WallTime{std::chrono::seconds{static_cast<std::int64_t>(*since)}};
    }
    for (const auto& [value, label] : known) {
      const std::optional<std::wstring> path =
          reg_string(HKEY_USERS, user + std::wstring(shell_folders), value);
      if (path && under(fs::path{expand_for(*path, profile->native())}, account.folder)) {
        account.protected_folders.emplace_back(label);
      }
    }
    accounts.push_back(std::move(account));
  }
  return accounts;
}

Result<std::vector<std::string>> Win32BackupSystem::installed_programs() const {
  const std::array<std::pair<HKEY, std::wstring>, 3> roots = {
      std::pair{HKEY_LOCAL_MACHINE, std::wstring(uninstall_path)},
      std::pair{HKEY_LOCAL_MACHINE, std::wstring(uninstall_path32)},
      std::pair{HKEY_USERS, widen(owner_sid_) + L"\\" + std::wstring(uninstall_path)}};
  std::vector<std::string> programs;
  for (const auto& [root, path] : roots) {
    Result<std::vector<std::wstring>> keys = subkeys(root, path);
    if (!keys) {
      return tl::unexpected(keys.error());
    }
    for (const std::wstring& key : *keys) {
      if (const std::optional<std::wstring> name =
              reg_string(root, std::wstring(path).append(L"\\").append(key), L"DisplayName");
          name && !name->empty()) {
        programs.push_back(narrow(*name));
      }
    }
  }
  return programs;
}

Result<std::vector<backup::Volume>> Win32BackupSystem::volumes() const {
  std::array<wchar_t, 512> drives{};
  const DWORD length = ::GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
  if (length == 0 || length > drives.size()) {
    return fail(Errc::io_error, "the drive letters could not be listed");
  }
  std::vector<backup::Volume> volumes;
  std::wstring_view rest(drives.data(), length);
  while (!rest.empty()) {
    const std::wstring root(rest.substr(0, rest.find(L'\0')));
    rest.remove_prefix(std::min(rest.size(), root.size() + 1));
    if (root.empty() || ::GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
      continue;
    }
    std::array<wchar_t, MAX_PATH + 1> label{};
    if (::GetVolumeInformationW(root.c_str(), label.data(), static_cast<DWORD>(label.size()),
                                nullptr, nullptr, nullptr, nullptr, 0) == FALSE) {
      continue;
    }
    volumes.push_back(backup::Volume{.root = fs::path{root}, .label = narrow(label.data())});
  }
  return volumes;
}

}  // namespace myclean::platform
