#include "platform/settings/system_space.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the COM and WMI headers need its types
#include <oleauto.h>
#include <propvarutil.h>
#include <wbemidl.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/settings.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace {

using settings_detail::narrow;
using settings_detail::win32_error;
using Row = wil::com_ptr_nothrow<IWbemClassObject>;

constexpr const wchar_t* power_key = LR"(SYSTEM\CurrentControlSet\Control\Power)";
constexpr DWORD hiber_file_type_reduced = 1;
constexpr long query_timeout_ms = 60'000;
constexpr std::uint64_t mebibyte = 1ULL << 20U;

tl::unexpected<Error> wmi_error(std::string_view what, HRESULT result) {
  const auto code = static_cast<std::uint32_t>(result);
  if (result == WBEM_E_ACCESS_DENIED || result == E_ACCESSDENIED ||
      result == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD)) {
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

// A connection to root\cimv2. Lives inside a ComScope.
class Wmi {
 public:
  [[nodiscard]] static Result<Wmi> connect() {
    wil::com_ptr_nothrow<IWbemLocator> locator;
    if (const HRESULT result = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(locator.put()));
        FAILED(result)) {
      return wmi_error("creating the WMI locator", result);
    }
    const wil::unique_bstr name_space{::SysAllocString(LR"(ROOT\CIMV2)")};
    Wmi wmi;
    if (const HRESULT result = locator->ConnectServer(name_space.get(), nullptr, nullptr, nullptr,
                                                      0, nullptr, nullptr, wmi.services_.put());
        FAILED(result)) {
      return wmi_error("connecting to WMI", result);
    }
    if (const HRESULT result = ::CoSetProxyBlanket(
            wmi.services_.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        FAILED(result)) {
      return wmi_error("setting the WMI proxy security", result);
    }
    return wmi;
  }

  [[nodiscard]] Result<std::vector<Row>> query(const wchar_t* wql) const {
    const wil::unique_bstr language{::SysAllocString(L"WQL")};
    const wil::unique_bstr text{::SysAllocString(wql)};
    wil::com_ptr_nothrow<IEnumWbemClassObject> rows;
    if (const HRESULT result = services_->ExecQuery(
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

  [[nodiscard]] IWbemServices& services() const { return *services_; }

 private:
  Wmi() = default;

  wil::com_ptr_nothrow<IWbemServices> services_;
};

std::optional<std::wstring> text_of(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr)) ||
      value.vt != VT_BSTR) {  // NOLINT(cppcoreguidelines-pro-type-union-access): the tag
    return std::nullopt;
  }
  return std::wstring{value.bstrVal};  // NOLINT(cppcoreguidelines-pro-type-union-access): VT_BSTR
}

// An unsigned property; uint64 values arrive as strings and are converted.
std::optional<std::uint64_t> number_of(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr))) {
    return std::nullopt;
  }
  const VARTYPE type = value.vt;  // NOLINT(cppcoreguidelines-pro-type-union-access): the tag
  ULONGLONG converted = 0;
  if (type == VT_NULL || type == VT_EMPTY || FAILED(::VariantToUInt64(value, &converted))) {
    return std::nullopt;
  }
  return converted;
}

std::optional<bool> flag_of(IWbemClassObject& row, const wchar_t* name) {
  wil::unique_variant value;
  if (FAILED(row.Get(name, 0, value.reset_and_addressof(), nullptr, nullptr))) {
    return std::nullopt;
  }
  const VARTYPE type = value.vt;  // NOLINT(cppcoreguidelines-pro-type-union-access): the tag
  BOOL converted = FALSE;
  if (type == VT_NULL || type == VT_EMPTY || FAILED(::VariantToBoolean(value, &converted))) {
    return std::nullopt;
  }
  return converted != FALSE;
}

std::optional<DWORD> registry_dword(const wchar_t* value) {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, power_key, value, RRF_RT_REG_DWORD, nullptr, &data,
                     &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return data;
}

Result<std::filesystem::path> system_root() {
  std::array<wchar_t, MAX_PATH> windows{};
  const UINT length = ::GetSystemWindowsDirectoryW(windows.data(), MAX_PATH);
  if (length < 3 || length >= MAX_PATH) {
    return win32_error("finding the Windows folder", ::GetLastError());
  }
  return std::filesystem::path{std::wstring{windows[0], L':', L'\\'}};
}

// The size of `file` from its directory entry: hiberfil.sys and pagefile.sys cannot be opened.
Bytes entry_size(const std::filesystem::path& file) {
  WIN32_FIND_DATAW entry{};
  const wil::unique_hfind find{
      ::FindFirstFileExW(file.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0)};
  if (!find) {
    return Bytes{};
  }
  return Bytes{(static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32U) | entry.nFileSizeLow};
}

Result<placement::PageFileState> read_page_files(const Wmi& wmi) {
  Result<std::vector<Row>> system =
      wmi.query(L"SELECT AutomaticManagedPagefile FROM Win32_ComputerSystem");
  if (!system) {
    return tl::unexpected(system.error());
  }
  placement::PageFileState state;
  state.config.automatic =
      !system->empty() && flag_of(*system->front(), L"AutomaticManagedPagefile").value_or(false);
  Result<std::vector<Row>> settings =
      wmi.query(L"SELECT Name, InitialSize, MaximumSize FROM Win32_PageFileSetting");
  if (!settings) {
    return tl::unexpected(settings.error());
  }
  for (const Row& row : *settings) {
    state.config.files.push_back(placement::PageFile{
        .path = text_of(*row, L"Name").value_or(L""),
        .initial_mb = static_cast<std::uint32_t>(number_of(*row, L"InitialSize").value_or(0)),
        .maximum_mb = static_cast<std::uint32_t>(number_of(*row, L"MaximumSize").value_or(0))});
  }
  Result<std::vector<Row>> usage =
      wmi.query(L"SELECT Name, AllocatedBaseSize FROM Win32_PageFileUsage");
  if (!usage) {
    return tl::unexpected(usage.error());
  }
  for (const Row& row : *usage) {
    state.in_use.push_back(placement::PageFileUse{
        .path = text_of(*row, L"Name").value_or(L""),
        .allocated = Bytes{number_of(*row, L"AllocatedBaseSize").value_or(0) * mebibyte}});
  }
  return state;
}

// The WMI object path of the page-file setting for `path`.
std::wstring setting_path(const std::filesystem::path& path) {
  std::wstring escaped;
  for (const wchar_t character : path.native()) {
    if (character == L'\\' || character == L'"') {
      escaped += L'\\';
    }
    escaped += character;
  }
  return LR"(Win32_PageFileSetting.Name=")" + escaped + L"\"";
}

Result<void> put(IWbemClassObject& row, const wchar_t* name, VARIANT& value) {
  if (const HRESULT result = row.Put(name, 0, &value, 0); FAILED(result)) {
    return wmi_error("setting " + narrow(name), result);
  }
  return {};
}

Result<void> set_automatic(const Wmi& wmi, bool automatic) {
  Result<std::vector<Row>> rows = wmi.query(L"SELECT * FROM Win32_ComputerSystem");
  if (!rows) {
    return tl::unexpected(rows.error());
  }
  if (rows->empty()) {
    return fail(Errc::not_found, "WMI lists no Win32_ComputerSystem");
  }
  wil::unique_variant value;
  ::InitVariantFromBoolean(automatic ? TRUE : FALSE, value.reset_and_addressof());
  if (Result<void> set = put(*rows->front(), L"AutomaticManagedPagefile", value); !set) {
    return set;
  }
  if (const HRESULT result =
          wmi.services().PutInstance(rows->front().get(), WBEM_FLAG_UPDATE_ONLY, nullptr, nullptr);
      FAILED(result)) {
    return wmi_error("changing AutomaticManagedPagefile", result);
  }
  return {};
}

Result<void> delete_setting(const Wmi& wmi, const std::filesystem::path& path) {
  const wil::unique_bstr object{::SysAllocString(setting_path(path).c_str())};
  if (const HRESULT result = wmi.services().DeleteInstance(object.get(), 0, nullptr, nullptr);
      FAILED(result)) {
    return wmi_error("deleting the page-file setting " + narrow(path.native()), result);
  }
  return {};
}

Result<void> put_setting(const Wmi& wmi, const placement::PageFile& file) {
  IWbemServices& services = wmi.services();
  const wil::unique_bstr object{::SysAllocString(setting_path(file.path).c_str())};
  Row row;
  if (FAILED(services.GetObject(object.get(), 0, nullptr, row.put(), nullptr))) {
    const wil::unique_bstr class_name{::SysAllocString(L"Win32_PageFileSetting")};
    Row definition;
    if (const HRESULT result =
            services.GetObject(class_name.get(), 0, nullptr, definition.put(), nullptr);
        FAILED(result)) {
      return wmi_error("reading the Win32_PageFileSetting class", result);
    }
    if (const HRESULT result = definition->SpawnInstance(0, row.put()); FAILED(result)) {
      return wmi_error("creating a page-file setting", result);
    }
    wil::unique_variant name;
    ::InitVariantFromString(file.path.c_str(), name.reset_and_addressof());
    if (Result<void> set = put(*row, L"Name", name); !set) {
      return set;
    }
  }
  wil::unique_variant initial;
  ::InitVariantFromInt32(static_cast<LONG>(file.initial_mb), initial.reset_and_addressof());
  wil::unique_variant maximum;
  ::InitVariantFromInt32(static_cast<LONG>(file.maximum_mb), maximum.reset_and_addressof());
  if (Result<void> set = put(*row, L"InitialSize", initial); !set) {
    return set;
  }
  if (Result<void> set = put(*row, L"MaximumSize", maximum); !set) {
    return set;
  }
  if (const HRESULT result =
          services.PutInstance(row.get(), WBEM_FLAG_CREATE_OR_UPDATE, nullptr, nullptr);
      FAILED(result)) {
    return wmi_error("writing the page-file setting " + narrow(file.path.native()), result);
  }
  return {};
}

// Changing page-file settings needs SeCreatePagefilePrivilege enabled in the caller's token.
Result<void> enable_pagefile_privilege() {
  wil::unique_handle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                         token.put()) == FALSE) {
    return win32_error("opening the process token", ::GetLastError());
  }
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (::LookupPrivilegeValueW(nullptr, SE_CREATE_PAGEFILE_NAME, &privileges.Privileges[0].Luid) ==
          FALSE ||
      ::AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) == FALSE ||
      ::GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
    return win32_error("enabling SeCreatePagefilePrivilege", ERROR_PRIVILEGE_NOT_HELD);
  }
  return {};
}

placement::HibernationMode hibernation_type(DWORD hiber_file_type) {
  return hiber_file_type == hiber_file_type_reduced ? placement::HibernationMode::reduced
                                                    : placement::HibernationMode::full;
}

bool elevated() {
  wil::unique_handle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE) {
    return false;
  }
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  return ::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) !=
             FALSE &&
         elevation.TokenIsElevated != 0;
}

// "C:" for a volume that has that drive letter, else its GUID path; from a Win32_Volume
// reference such as Win32_Volume.DeviceID="\\\\?\\Volume{...}\\".
std::string volume_name(const std::wstring& reference) {
  const std::size_t first = reference.find(L'"');
  const std::size_t last = reference.rfind(L'"');
  std::wstring volume;
  if (first != std::wstring::npos && last > first) {
    const std::wstring_view quoted =
        std::wstring_view{reference}.substr(first + 1, last - first - 1);
    for (std::size_t index = 0; index < quoted.size(); ++index) {
      if (quoted[index] == L'\\' && index + 1 < quoted.size()) {
        ++index;
      }
      volume += quoted[index];
    }
  }
  std::array<wchar_t, MAX_PATH> names{};
  DWORD needed = 0;
  if (::GetVolumePathNamesForVolumeNameW(volume.c_str(), names.data(),
                                         static_cast<DWORD>(names.size()), &needed) != FALSE) {
    const std::wstring first_name{names.data()};
    if (first_name.size() == 3 && first_name[1] == L':') {
      return narrow(first_name.substr(0, 2));
    }
  }
  return narrow(volume);
}

}  // namespace

Result<placement::HibernationState> Win32SystemSpace::hibernation() const {
  Result<std::filesystem::path> root = system_root();
  if (!root) {
    return tl::unexpected(root.error());
  }
  const DWORD enabled = registry_dword(L"HibernateEnabled")
                            .value_or(registry_dword(L"HibernateEnabledDefault").value_or(1));
  const DWORD type = registry_dword(L"HiberFileType").value_or(0);
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (::GlobalMemoryStatusEx(&memory) == FALSE) {
    return win32_error("reading the memory size", ::GetLastError());
  }
  placement::HibernationState state{
      .mode = enabled == 0 ? placement::HibernationMode::off : hibernation_type(type),
      .size_percent = registry_dword(L"HiberFileSizePercent").value_or(0),
      .file = *root / "hiberfil.sys",
      .memory = Bytes{memory.ullTotalPhys}};
  state.file_size = entry_size(state.file);
  return state;
}

Result<placement::PageFileState> Win32SystemSpace::page_files() const {
  const ComScope com;
  if (Result<void> ready = com.status(); !ready) {
    return tl::unexpected(ready.error());
  }
  Result<Wmi> wmi = Wmi::connect();
  if (!wmi) {
    return tl::unexpected(wmi.error());
  }
  return read_page_files(*wmi);
}

Result<void> Win32SystemSpace::set_page_files(const placement::PageFileConfig& config) {
  const ComScope com;
  if (Result<void> ready = com.status(); !ready) {
    return ready;
  }
  if (Result<void> privilege = enable_pagefile_privilege(); !privilege) {
    return privilege;
  }
  Result<Wmi> wmi = Wmi::connect();
  if (!wmi) {
    return tl::unexpected(wmi.error());
  }
  Result<placement::PageFileState> current = read_page_files(*wmi);
  if (!current) {
    return tl::unexpected(current.error());
  }
  const placement::PageFileConfig& before = current->config;
  if (!config.automatic && before.automatic) {
    if (Result<void> set = set_automatic(*wmi, false); !set) {
      return set;
    }
  }
  for (const placement::PageFile& file : before.files) {
    const bool kept = std::ranges::any_of(config.files, [&](const placement::PageFile& wanted) {
      return placement::same_path(wanted.path, file.path);
    });
    if (!kept) {
      if (Result<void> deleted = delete_setting(*wmi, file.path); !deleted) {
        return deleted;
      }
    }
  }
  for (const placement::PageFile& file : config.files) {
    if (std::ranges::find(before.files, file) == before.files.end()) {
      if (Result<void> written = put_setting(*wmi, file); !written) {
        return written;
      }
    }
  }
  if (config.automatic && !before.automatic) {
    return set_automatic(*wmi, true);
  }
  return {};
}

Result<std::vector<placement::ShadowStorage>> Win32SystemSpace::shadow_storage() const {
  const ComScope com;
  if (Result<void> ready = com.status(); !ready) {
    return tl::unexpected(ready.error());
  }
  Result<Wmi> wmi = Wmi::connect();
  if (!wmi) {
    return tl::unexpected(wmi.error());
  }
  Result<std::vector<Row>> rows = wmi->query(
      L"SELECT Volume, DiffVolume, UsedSpace, AllocatedSpace, MaxSpace FROM Win32_ShadowStorage");
  if (!rows) {
    // Without elevation the shadow-copy provider fails to initialize instead of denying access.
    if (elevated()) {
      return tl::unexpected(rows.error());
    }
    return fail(Errc::access_denied,
                "reading the shadow storage needs administrator rights: " + rows.error().message);
  }
  std::vector<placement::ShadowStorage> storages;
  for (const Row& row : *rows) {
    const std::uint64_t maximum =
        number_of(*row, L"MaxSpace").value_or(std::numeric_limits<std::uint64_t>::max());
    storages.push_back(placement::ShadowStorage{
        .volume = volume_name(text_of(*row, L"Volume").value_or(L"")),
        .diff_volume = volume_name(text_of(*row, L"DiffVolume").value_or(L"")),
        .used = Bytes{number_of(*row, L"UsedSpace").value_or(0)},
        .allocated = Bytes{number_of(*row, L"AllocatedSpace").value_or(0)},
        .maximum = maximum == std::numeric_limits<std::uint64_t>::max()
                       ? std::nullopt
                       : std::optional<Bytes>{Bytes{maximum}}});
  }
  return storages;
}

}  // namespace myclean::platform
