#include "platform/scan/installer_inspector.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <msi.h>
#include <msiquery.h>
#include <winver.h>

#include "core/model/result.hpp"
#include "core/space/installers.hpp"
#include "platform/scan/scan_support.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using scan_support::narrow;
using scan_support::os_error;

constexpr const wchar_t* uninstall_key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// A string value of `subkey`; empty when it is missing or not a string.
std::wstring registry_string(HKEY key, const wchar_t* subkey, const wchar_t* name) {
  DWORD bytes = 0;
  if (::RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return {};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(key, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) !=
      ERROR_SUCCESS) {
    return {};
  }
  value.resize(::wcsnlen(value.c_str(), value.size()));
  return value;
}

// One string of the version resource in `data`, in its first language; empty when missing.
std::wstring version_string(std::vector<std::byte>& data, const wchar_t* name) {
  struct Translation {
    WORD language;
    WORD code_page;
  };
  std::vector<std::wstring> blocks;
  void* pointer = nullptr;
  UINT bytes = 0;
  if (::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &pointer, &bytes) != FALSE &&
      bytes >= sizeof(Translation)) {
    Translation first{};
    std::memcpy(&first, pointer, sizeof(first));
    blocks.push_back(std::format(L"{:04x}{:04x}", first.language, first.code_page));
  }
  blocks.emplace_back(L"040904b0");  // US English, Unicode: the usual fallback
  blocks.emplace_back(L"040904e4");  // US English, Windows-1252
  for (const std::wstring& block : blocks) {
    const std::wstring query = L"\\StringFileInfo\\" + block + L"\\" + name;
    if (::VerQueryValueW(data.data(), query.c_str(), &pointer, &bytes) != FALSE && bytes > 0) {
      const auto* text = static_cast<const wchar_t*>(pointer);
      return {text, ::wcsnlen(text, bytes)};
    }
  }
  return {};
}

}  // namespace

Result<std::optional<std::string>> Win32InstallerInspector::msi_product_code(
    const fs::path& package) const {
  PMSIHANDLE database;
  // A null persist mode is MSIDBOPEN_READONLY: the package is never written.
  UINT result = ::MsiOpenDatabaseW(package.c_str(), nullptr, &database);
  if (result != ERROR_SUCCESS) {
    return tl::unexpected(os_error(result, "cannot open the package", package));
  }
  PMSIHANDLE view;
  result = ::MsiDatabaseOpenViewW(
      database, L"SELECT `Value` FROM `Property` WHERE `Property`='ProductCode'", &view);
  if (result == ERROR_SUCCESS) {
    result = ::MsiViewExecute(view, 0);
  }
  if (result != ERROR_SUCCESS) {
    return tl::unexpected(os_error(result, "cannot read the Property table of", package));
  }
  PMSIHANDLE record;
  result = ::MsiViewFetch(view, &record);
  if (result == ERROR_NO_MORE_ITEMS) {
    return std::optional<std::string>{};
  }
  std::array<wchar_t, 64> code{};  // a GUID in braces is 38 characters
  auto length = static_cast<DWORD>(code.size());
  if (result == ERROR_SUCCESS) {
    result = ::MsiRecordGetStringW(record, 1, code.data(), &length);
  }
  if (result != ERROR_SUCCESS) {
    return tl::unexpected(os_error(result, "cannot read the ProductCode of", package));
  }
  return std::optional<std::string>{narrow(std::wstring(code.data(), length))};
}

Result<std::optional<std::string>> Win32InstallerInspector::installed_msi_product(
    const std::string& product_code) const {
  const std::wstring code = scan_support::wide(product_code);
  const INSTALLSTATE state = ::MsiQueryProductStateW(code.c_str());
  if (state == INSTALLSTATE_INVALIDARG) {
    return fail(Errc::invalid_data, "not a product code: " + product_code);
  }
  if (state != INSTALLSTATE_DEFAULT) {
    return std::optional<std::string>{};
  }
  std::array<wchar_t, 512> name{};
  auto length = static_cast<DWORD>(name.size());
  if (::MsiGetProductInfoW(code.c_str(), L"InstalledProductName", name.data(), &length) !=
          ERROR_SUCCESS ||
      length == 0) {
    return std::optional<std::string>{product_code};
  }
  return std::optional<std::string>{narrow(std::wstring(name.data(), length))};
}

Result<std::optional<space::VersionInfo>> Win32InstallerInspector::exe_version(
    const fs::path& executable) const {
  DWORD ignored = 0;
  const DWORD size =
      ::GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, executable.c_str(), &ignored);
  if (size == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_RESOURCE_DATA_NOT_FOUND || error == ERROR_RESOURCE_TYPE_NOT_FOUND ||
        error == ERROR_RESOURCE_NAME_NOT_FOUND || error == ERROR_BAD_EXE_FORMAT) {
      return std::optional<space::VersionInfo>{};
    }
    return tl::unexpected(os_error(error, "cannot read the version of", executable));
  }
  std::vector<std::byte> data(size);
  if (::GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, executable.c_str(), 0, size, data.data()) ==
      FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot read the version of", executable));
  }
  return std::optional<space::VersionInfo>{
      space::VersionInfo{.product_name = narrow(version_string(data, L"ProductName")),
                         .company_name = narrow(version_string(data, L"CompanyName"))}};
}

Result<std::vector<space::InstalledProgram>> Win32InstallerInspector::installed_programs() const {
  struct Root {
    HKEY hive;
    REGSAM view;
  };
  const std::array<Root, 3> roots = {Root{.hive = HKEY_LOCAL_MACHINE, .view = KEY_WOW64_64KEY},
                                     Root{.hive = HKEY_LOCAL_MACHINE, .view = KEY_WOW64_32KEY},
                                     Root{.hive = HKEY_CURRENT_USER, .view = 0}};
  std::vector<space::InstalledProgram> programs;
  for (const Root& root : roots) {
    wil::unique_hkey key;
    const LSTATUS status = ::RegOpenKeyExW(root.hive, uninstall_key, 0, KEY_READ | root.view, &key);
    if (status == ERROR_FILE_NOT_FOUND) {
      continue;
    }
    if (status != ERROR_SUCCESS) {
      return tl::unexpected(os_error(static_cast<DWORD>(status), "cannot open", uninstall_key));
    }
    for (DWORD index = 0;; ++index) {
      std::array<wchar_t, 256> name{};  // registry key names have at most 255 characters
      auto length = static_cast<DWORD>(name.size());
      const LSTATUS next = ::RegEnumKeyExW(key.get(), index, name.data(), &length, nullptr, nullptr,
                                           nullptr, nullptr);
      if (next == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (next != ERROR_SUCCESS) {
        continue;
      }
      const std::wstring display = registry_string(key.get(), name.data(), L"DisplayName");
      if (display.empty()) {
        continue;
      }
      programs.push_back(space::InstalledProgram{
          .display_name = narrow(display),
          .publisher = narrow(registry_string(key.get(), name.data(), L"Publisher")),
          .display_version = narrow(registry_string(key.get(), name.data(), L"DisplayVersion"))});
    }
  }
  return programs;
}

}  // namespace myclean::platform
