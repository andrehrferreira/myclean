#include "platform/packs/win32_packs.hpp"

// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
// clang-format on

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wil/resource.h>

#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view uninstall_key =
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr std::wstring_view packages_key =
    LR"(Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages)";

// Owners and trustees that may write: SYSTEM, Administrators, TrustedInstaller.
constexpr std::array<std::wstring_view, 3> trusted_sids = {
    L"S-1-5-18", L"S-1-5-32-544",
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464"};
constexpr std::wstring_view creator_owner_sid = L"S-1-3-0";

constexpr ACCESS_MASK write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                     FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE |
                                     WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
  return result;
}

std::string path_utf8(const fs::path& path) {
  return utf8(std::wstring_view{path.native()});
}

Error win32_error(std::string_view what, DWORD code) {
  return Error{
      .code = Errc::io_error,
      .message = std::string(what) + ": " + std::system_category().message(static_cast<int>(code))};
}

wil::unique_hkey open_key(HKEY root, std::wstring_view path, REGSAM view = 0) {
  wil::unique_hkey key;
  if (::RegOpenKeyExW(root, std::wstring(path).c_str(), 0, KEY_READ | view, key.put()) !=
      ERROR_SUCCESS) {
    key.reset();
  }
  return key;
}

std::vector<std::wstring> subkeys(HKEY key) {
  std::vector<std::wstring> names;
  std::array<wchar_t, 256> name{};  // registry key names are at most 255 characters
  for (DWORD index = 0;; ++index) {
    auto length = static_cast<DWORD>(name.size());
    const LSTATUS status =
        ::RegEnumKeyExW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS || (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)) {
      break;
    }
    if (status == ERROR_SUCCESS) {
      names.emplace_back(name.data(), length);
    }
  }
  return names;
}

// The string value `name` of `key\subkey`; nullopt when it is missing or not a string.
std::optional<std::wstring> string_value(HKEY key, const std::wstring& subkey,
                                         const wchar_t* name) {
  DWORD bytes = 0;
  if (::RegGetValueW(key, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return std::nullopt;
  }
  std::wstring text(bytes / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(key, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  text.resize(bytes / sizeof(wchar_t));
  while (!text.empty() && text.back() == L'\0') {
    text.pop_back();
  }
  return text;
}

void add_display_names(HKEY root, REGSAM view, std::vector<std::string>& apps) {
  const wil::unique_hkey key = open_key(root, uninstall_key, view);
  if (!key) {
    return;
  }
  for (const std::wstring& subkey : subkeys(key.get())) {
    if (std::optional<std::wstring> name = string_value(key.get(), subkey, L"DisplayName");
        name && !name->empty()) {
      apps.push_back(utf8(std::wstring_view{*name}));
    }
  }
}

std::wstring sid_text(PSID sid) {
  wil::unique_hlocal_string text;
  if (sid == nullptr || ::ConvertSidToStringSidW(sid, text.put()) == FALSE) {
    return {};
  }
  return text.get();
}

bool trusted(std::wstring_view sid) {
  return std::ranges::find(trusted_sids, sid) != trusted_sids.end();
}

Result<void> check_descriptor(PSECURITY_DESCRIPTOR descriptor, std::string_view what) {
  PSID owner = nullptr;
  BOOL defaulted = FALSE;
  BOOL present = FALSE;
  PACL dacl = nullptr;
  if (::GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) == FALSE ||
      ::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE) {
    return tl::unexpected(win32_error("reading the security descriptor", ::GetLastError()));
  }
  const auto refused = [what](std::string_view problem) {
    return fail(Errc::refused, std::string(what) + ": " + std::string(problem));
  };
  if (const std::wstring sid = sid_text(owner); !trusted(sid)) {
    return refused("is owned by " + utf8(std::wstring_view{sid}) +
                   ", not by SYSTEM or Administrators");
  }
  if (present == FALSE || dacl == nullptr) {
    return refused("has no access control list: everyone may write");
  }
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* entry = nullptr;
    if (::GetAce(dacl, index, &entry) == FALSE) {
      return tl::unexpected(win32_error("reading an access control entry", ::GetLastError()));
    }
    const auto* header = static_cast<const ACE_HEADER*>(entry);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0) {
      continue;
    }
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
    const std::wstring sid =
        sid_text(const_cast<DWORD*>(&allowed->SidStart));  // NOLINT(*-const-cast)
    if ((allowed->Mask & write_rights) != 0 && !trusted(sid) && sid != creator_owner_sid) {
      return refused(utf8(std::wstring_view{sid}) + " may write to it");
    }
  }
  return {};
}

Result<void> check_entry(const fs::path& path) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (const DWORD error = ::GetNamedSecurityInfoW(
          path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          nullptr, nullptr, nullptr, nullptr, &raw);
      error != ERROR_SUCCESS) {
    return tl::unexpected(win32_error("reading the permissions of " + path_utf8(path), error));
  }
  const wil::unique_hlocal_security_descriptor descriptor(raw);
  return check_descriptor(descriptor.get(), path_utf8(path));
}

}  // namespace

std::vector<std::string> Win32Machine::installed_apps() const {
  std::vector<std::string> apps;
  add_display_names(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, apps);
  add_display_names(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, apps);
  if (user_sid_.empty()) {
    add_display_names(HKEY_CURRENT_USER, 0, apps);
  } else if (const wil::unique_hkey user = open_key(HKEY_USERS, user_sid_)) {
    add_display_names(user.get(), 0, apps);
  }
  // The user's classes: HKCU\Software\Classes, loaded for another user as <sid>_Classes.
  const wil::unique_hkey classes = user_sid_.empty()
                                       ? open_key(HKEY_CURRENT_USER, L"Software\\Classes")
                                       : open_key(HKEY_USERS, user_sid_ + L"_Classes");
  if (classes) {
    if (const wil::unique_hkey packages = open_key(classes.get(), packages_key)) {
      for (const std::wstring& name : subkeys(packages.get())) {
        apps.push_back(utf8(std::wstring_view{name}));
      }
    }
  }
  return apps;
}

std::vector<std::string> Win32Machine::running_images() const {
  std::vector<std::string> images;
  const wil::unique_hfile snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot) {
    return images;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more != FALSE;
       more = ::Process32NextW(snapshot.get(), &entry)) {
    images.push_back(utf8(std::wstring_view{static_cast<const wchar_t*>(entry.szExeFile)}));
  }
  return images;
}

std::uint32_t Win32Machine::os_build() const {
  const wil::unique_hkey key =
      open_key(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)");
  if (!key) {
    return 0;
  }
  const std::optional<std::wstring> build = string_value(key.get(), L"", L"CurrentBuildNumber");
  if (!build) {
    return 0;
  }
  std::uint32_t number = 0;
  for (const wchar_t digit : *build) {
    if (digit < L'0' || digit > L'9') {
      return 0;
    }
    number = (number * 10) + static_cast<std::uint32_t>(digit - L'0');
  }
  return number;
}

Result<void> check_admin_write_only(std::wstring_view sddl) {
  wil::unique_hlocal_security_descriptor descriptor;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          std::wstring(sddl).c_str(), SDDL_REVISION_1, wil::out_param(descriptor), nullptr) ==
      FALSE) {
    return tl::unexpected(win32_error("parsing the security descriptor", ::GetLastError()));
  }
  return check_descriptor(descriptor.get(), "the security descriptor");
}

Result<fs::path> owner_packs_dir() {
  wil::unique_cotaskmem_string folder;
  if (const HRESULT result =
          ::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, folder.put());
      FAILED(result)) {
    return fail(Errc::io_error, "the ProgramData folder is unknown");
  }
  return fs::path{folder.get()} / "MyClean" / "packs";
}

Result<std::optional<fs::path>> owner_packs_folder(const fs::path& folder) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(folder, error);
  if (status.type() == fs::file_type::not_found) {
    return std::optional<fs::path>{};
  }
  if (error) {
    return fail(Errc::io_error, path_utf8(folder) + ": " + error.message());
  }
  const auto check = [](const fs::path& path, const fs::file_status& kind) -> Result<void> {
    if (fs::is_symlink(kind) ||
        (::GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(Errc::refused, path_utf8(path) + ": is a link; owner packs are ignored");
    }
    return check_entry(path);
  };
  if (Result<void> checked = check(folder, status); !checked) {
    return fail(checked.error().code, "owner packs ignored: " + checked.error().message);
  }
  for (fs::recursive_directory_iterator entry(folder, error), end; !error && entry != end;
       entry.increment(error)) {
    if (Result<void> checked = check(entry->path(), entry->symlink_status(error)); !checked) {
      return fail(checked.error().code, "owner packs ignored: " + checked.error().message);
    }
  }
  if (error) {
    return fail(Errc::io_error,
                "owner packs ignored: " + path_utf8(folder) + ": " + error.message());
  }
  return std::optional<fs::path>{folder};
}

}  // namespace myclean::platform
