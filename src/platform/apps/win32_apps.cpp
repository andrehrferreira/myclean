#include "platform/apps/win32_apps.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>

// clang-format off
#include <windows.h>  // first: the shell and WinRT headers need its types
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>
// clang-format on

#include "core/apps/apps.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view user_assist_key =
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist)";
constexpr std::wstring_view bam_key =
    LR"(SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\)";
constexpr std::size_t max_prefetch = 16U << 20U;  // bytes; Prefetch files are far smaller
constexpr USHORT xpress_huff = 4;                 // COMPRESSION_FORMAT_XPRESS_HUFF

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
  } else if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    code = Errc::not_found;
  }
  return fail(code, std::format("{}: Win32 error {}", what, status));
}

// Every value of `key`: name and bytes.
Result<std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>>> values_of(
    HKEY root, const std::wstring& key) {
  wil::unique_hkey handle;
  const LSTATUS opened =
      ::RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, handle.put());
  if (opened != ERROR_SUCCESS) {
    return win32_failed("opening " + narrow(key), opened);
  }
  DWORD max_name = 0;
  DWORD max_data = 0;
  if (const LSTATUS info =
          ::RegQueryInfoKeyW(handle.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, &max_name, &max_data, nullptr, nullptr);
      info != ERROR_SUCCESS) {
    return win32_failed("reading " + narrow(key), info);
  }
  std::vector<std::pair<std::wstring, std::vector<std::uint8_t>>> values;
  for (DWORD index = 0;; ++index) {
    std::wstring name(max_name + 1, L'\0');
    std::vector<std::uint8_t> data(max_data);
    DWORD name_length = max_name + 1;
    DWORD data_length = max_data;
    DWORD type = 0;
    const LSTATUS status = ::RegEnumValueW(handle.get(), index, name.data(), &name_length, nullptr,
                                           &type, data.data(), &data_length);
    if (status == ERROR_NO_MORE_ITEMS) {
      return values;
    }
    if (status != ERROR_SUCCESS) {
      return win32_failed("listing " + narrow(key), status);
    }
    if (type != REG_BINARY) {
      continue;
    }
    name.resize(name_length);
    data.resize(data_length);
    values.emplace_back(std::move(name), std::move(data));
  }
}

Result<std::vector<std::wstring>> subkeys_of(HKEY root, const std::wstring& key) {
  wil::unique_hkey handle;
  const LSTATUS opened =
      ::RegOpenKeyExW(root, key.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, handle.put());
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

std::optional<std::wstring> reg_string(HKEY root, const std::wstring& key, const wchar_t* name) {
  DWORD size = 0;
  constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY;
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

std::uint64_t u64_of(std::span<const std::uint8_t> data) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value) && index < data.size(); ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8U * index);
  }
  return value;
}

std::optional<fs::path> known_folder(const KNOWNFOLDERID& id) {
  PWSTR path = nullptr;
  const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path);
  std::optional<fs::path> folder;
  if (SUCCEEDED(result) && path != nullptr) {
    folder = fs::path{std::wstring_view(path)};
  }
  ::CoTaskMemFree(path);
  return folder;
}

// The owner's profile folder (ProfileList\<sid>\ProfileImagePath, expanded).
std::optional<fs::path> profile_of(const std::string& sid) {
  const std::optional<std::wstring> path =
      reg_string(HKEY_LOCAL_MACHINE,
                 LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\)" + widen(sid),
                 L"ProfileImagePath");
  if (!path) {
    return std::nullopt;
  }
  std::array<wchar_t, MAX_PATH> expanded{};
  const DWORD length = ::ExpandEnvironmentStringsW(path->c_str(), expanded.data(), MAX_PATH);
  if (length == 0 || length > MAX_PATH) {
    return fs::path{*path};
  }
  return fs::path{expanded.data()};
}

// The folders UserAssist names by their KNOWNFOLDERID, upper-case "{GUID}" -> folder.
std::map<std::wstring, fs::path> known_folders(const std::string& owner_sid) {
  std::map<std::wstring, fs::path> folders;
  const std::array<std::pair<const wchar_t*, const KNOWNFOLDERID*>, 7> machine = {
      std::pair{L"{6D809377-6AF0-444B-8957-A3773F02200E}", &FOLDERID_ProgramFilesX64},
      std::pair{L"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}", &FOLDERID_ProgramFilesX86},
      std::pair{L"{905E63B6-C1BF-494E-B29C-65B732D3D21A}", &FOLDERID_ProgramFiles},
      std::pair{L"{F7F1ED05-9F6D-47A2-AAAE-29D317C6F066}", &FOLDERID_ProgramFilesCommon},
      std::pair{L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}", &FOLDERID_System},
      std::pair{L"{D65231B0-B2F1-4857-A4CE-A8E7C6EA7D27}", &FOLDERID_SystemX86},
      std::pair{L"{F38BF404-1D43-42F2-9305-67DE0B28FC23}", &FOLDERID_Windows}};
  for (const auto& [guid, id] : machine) {
    if (std::optional<fs::path> folder = known_folder(*id)) {
      folders[guid] = *std::move(folder);
    }
  }
  if (const std::optional<fs::path> profile = profile_of(owner_sid)) {
    folders[L"{5E6C858F-0E22-4760-9AFE-EA3317B67173}"] = *profile;
    folders[L"{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}"] = *profile / L"AppData" / L"Local";
    folders[L"{3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}"] = *profile / L"AppData" / L"Roaming";
  }
  return folders;
}

// The bytes of file `path`, at most max_prefetch.
Result<std::vector<std::uint8_t>> read_file(const fs::path& path) {
  const wil::unique_hfile file{::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!file) {
    return win32_failed("opening " + narrow(path.native()), static_cast<LSTATUS>(::GetLastError()));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > max_prefetch) {
    return fail(Errc::invalid_data, "unexpected size of " + narrow(path.native()));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  if (::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ==
      FALSE) {
    return win32_failed("reading " + narrow(path.native()), static_cast<LSTATUS>(::GetLastError()));
  }
  bytes.resize(read);
  return bytes;
}

using DecompressBufferEx = LONG(WINAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
using WorkSpaceSize = LONG(WINAPI*)(USHORT, PULONG, PULONG);

// A Prefetch file as SCCA bytes: compressed ones ("MAM\x04", the uncompressed size, then
// XPRESS_HUFF data) decompressed.
Result<std::vector<std::uint8_t>> decompressed(std::vector<std::uint8_t> bytes) {
  constexpr std::size_t header = 8;
  if (bytes.size() < header || bytes[0] != 'M' || bytes[1] != 'A' || bytes[2] != 'M' ||
      bytes[3] != 4) {
    return bytes;
  }
  const std::uint32_t size = static_cast<std::uint32_t>(u64_of(std::span(bytes).subspan(4, 4)));
  if (size > max_prefetch) {
    return fail(Errc::invalid_data, "a Prefetch file larger than expected");
  }
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
  const auto decompress =
      reinterpret_cast<DecompressBufferEx>(::GetProcAddress(ntdll, "RtlDecompressBufferEx"));
  const auto work_space =
      reinterpret_cast<WorkSpaceSize>(::GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize"));
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  if (decompress == nullptr || work_space == nullptr) {
    return fail(Errc::io_error, "ntdll has no RtlDecompressBufferEx");
  }
  ULONG buffer_size = 0;
  ULONG fragment_size = 0;
  if (work_space(xpress_huff, &buffer_size, &fragment_size) != 0) {
    return fail(Errc::io_error, "RtlGetCompressionWorkSpaceSize failed");
  }
  std::vector<std::uint8_t> work(buffer_size);
  std::vector<std::uint8_t> output(size);
  ULONG final_size = 0;
  const std::span<std::uint8_t> compressed = std::span(bytes).subspan(header);
  if (decompress(xpress_huff, output.data(), size, compressed.data(),
                 static_cast<ULONG>(compressed.size()), &final_size, work.data()) != 0) {
    return fail(Errc::invalid_data, "a Prefetch file that does not decompress");
  }
  output.resize(final_size);
  return output;
}

// Volume serial number -> "C:" for every drive letter.
std::map<std::uint32_t, std::string> drives_by_serial() {
  std::map<std::uint32_t, std::string> drives;
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const std::wstring root = std::wstring(1, letter) + L":\\";
    DWORD serial = 0;
    if (::GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) !=
        FALSE) {
      drives[serial] = narrow(root.substr(0, 2));
    }
  }
  return drives;
}

// Device path ("\Device\HarddiskVolume3", lower case) -> "C:" for every drive letter.
std::map<std::wstring, std::wstring> drives_by_device() {
  std::map<std::wstring, std::wstring> drives;
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const std::wstring drive = std::wstring(1, letter) + L":";
    std::array<wchar_t, MAX_PATH> device{};
    if (::QueryDosDeviceW(drive.c_str(), device.data(), MAX_PATH) != 0) {
      drives[lower(device.data())] = drive;
    }
  }
  return drives;
}

// The bytes of the files below `folder`, no reparse point followed.
std::uint64_t folder_size(const fs::path& folder) {
  std::uint64_t total = 0;
  std::vector<fs::path> pending{folder};
  while (!pending.empty()) {
    const fs::path next = std::move(pending.back());
    pending.pop_back();
    WIN32_FIND_DATAW data{};
    const wil::unique_hfind find{::FindFirstFileExW((next / L"*").c_str(), FindExInfoBasic, &data,
                                                    FindExSearchNameMatch, nullptr,
                                                    FIND_FIRST_EX_LARGE_FETCH)};
    if (!find) {
      continue;
    }
    for (bool more = true; more; more = ::FindNextFileW(find.get(), &data) != FALSE) {
      const std::wstring_view name(static_cast<const wchar_t*>(data.cFileName));
      if (name == L"." || name == L"..") {
        continue;
      }
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        continue;
      }
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        pending.push_back(next / name);
      } else {
        total += (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
      }
    }
  }
  return total;
}

std::string current_sid() {
  wil::unique_handle token;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE) {
    return {};
  }
  std::array<std::uint8_t, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> buffer{};
  DWORD size = 0;
  if (::GetTokenInformation(token.get(), TokenUser, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &size) == FALSE) {
    return {};
  }
  TOKEN_USER user{};
  std::memcpy(&user, buffer.data(), sizeof(user));
  LPWSTR text = nullptr;
  if (::ConvertSidToStringSidW(user.User.Sid, &text) == FALSE) {
    return {};
  }
  std::string sid = narrow(text);
  ::LocalFree(text);
  return sid;
}

}  // namespace

Result<std::vector<apps::AppxPackage>> Win32AppSystem::appx() const {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error&) {  // NOLINT(bugprone-empty-catch): a thread's other mode
  }
  std::vector<apps::AppxPackage> packages;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    // The caller's own packages need no rights; another user's need administrator rights.
    const std::wstring user = owner_sid_ == current_sid() ? std::wstring{} : widen(owner_sid_);
    for (const winrt::Windows::ApplicationModel::Package& package :
         manager.FindPackagesForUser(user)) {
      if (package.IsFramework() || package.IsResourcePackage()) {
        continue;
      }
      const winrt::Windows::ApplicationModel::PackageId id = package.Id();
      const winrt::Windows::ApplicationModel::PackageVersion version = id.Version();
      apps::AppxPackage entry{
          .full_name = winrt::to_string(id.FullName()),
          .family_name = winrt::to_string(id.FamilyName()),
          .version = std::format("{}.{}.{}.{}", version.Major, version.Minor, version.Build,
                                 version.Revision),
          .system = package.SignatureKind() ==
                    winrt::Windows::ApplicationModel::PackageSignatureKind::System};
      try {
        entry.display_name = winrt::to_string(package.DisplayName());
        entry.publisher = winrt::to_string(package.PublisherDisplayName());
        entry.location = fs::path{std::wstring(package.InstalledLocation().Path())};
        entry.installed = std::chrono::time_point_cast<WallTime::duration>(
            winrt::clock::to_sys(package.InstalledDate()));
      } catch (const winrt::hresult_error&) {  // NOLINT(bugprone-empty-catch): fields stay empty
      }
      if (!entry.location.empty()) {
        const std::scoped_lock lock(mutex_);
        const auto [size, added] = sizes_.try_emplace(entry.full_name);
        if (added) {
          size->second = Bytes{folder_size(entry.location)};
        }
        entry.size = size->second;
      }
      packages.push_back(std::move(entry));
    }
  } catch (const winrt::hresult_error& error) {
    const auto code = static_cast<std::uint32_t>(error.code().value);
    return fail(error.code() == E_ACCESSDENIED ? Errc::access_denied : Errc::io_error,
                std::format("PackageManager.FindPackagesForUser failed (0x{:08x}): {}", code,
                            winrt::to_string(error.message())));
  }
  return packages;
}

Result<std::vector<apps::RunRecord>> Win32AppSystem::user_assist() const {
  const std::wstring root = widen(owner_sid_) + L"\\" + std::wstring(user_assist_key);
  Result<std::vector<std::wstring>> guids = subkeys_of(HKEY_USERS, root);
  if (!guids) {
    return tl::unexpected(guids.error());
  }
  const std::map<std::wstring, fs::path> folders = known_folders(owner_sid_);
  constexpr std::size_t guid_length = 38;
  std::vector<apps::RunRecord> records;
  for (const std::wstring& guid : *guids) {
    std::wstring count_key = root;
    count_key.append(L"\\").append(guid).append(L"\\Count");
    auto values = values_of(HKEY_USERS, count_key);
    if (!values) {
      continue;
    }
    for (const auto& [name, data] : *values) {
      const std::optional<WallTime> time = apps::user_assist_time(data);
      if (!time) {
        continue;
      }
      std::string path = apps::rot13(narrow(name));
      if (path.size() > guid_length && path.front() == '{' && path[guid_length - 1] == '}') {
        std::wstring known = widen(path.substr(0, guid_length));
        std::ranges::transform(known, known.begin(), [](wchar_t c) {
          return static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(c)));
        });
        if (const auto folder = folders.find(known); folder != folders.end()) {
          path = narrow(folder->second.native()) + path.substr(guid_length);
        }
      }
      records.push_back(apps::RunRecord{
          .source = apps::UsageSource::user_assist, .path = std::move(path), .last_run = *time});
    }
  }
  return records;
}

Result<std::vector<apps::RunRecord>> Win32AppSystem::prefetch() const {
  std::array<wchar_t, MAX_PATH> windows{};
  const UINT length = ::GetWindowsDirectoryW(windows.data(), MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return fail(Errc::io_error, "the Windows folder could not be read");
  }
  const fs::path folder = fs::path{std::wstring_view(windows.data(), length)} / L"Prefetch";
  WIN32_FIND_DATAW data{};
  const wil::unique_hfind find{::FindFirstFileExW((folder / L"*.pf").c_str(), FindExInfoBasic,
                                                  &data, FindExSearchNameMatch, nullptr, 0)};
  if (!find) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return std::vector<apps::RunRecord>{};
    }
    return win32_failed(error == ERROR_ACCESS_DENIED
                            ? "listing the Prefetch folder (needs administrator rights)"
                            : "listing the Prefetch folder",
                        static_cast<LSTATUS>(error));
  }
  const std::map<std::uint32_t, std::string> drives = drives_by_serial();
  std::vector<apps::RunRecord> records;
  for (bool more = true; more; more = ::FindNextFileW(find.get(), &data) != FALSE) {
    Result<std::vector<std::uint8_t>> bytes =
        read_file(folder / static_cast<const wchar_t*>(data.cFileName));
    if (!bytes) {
      continue;
    }
    Result<std::vector<std::uint8_t>> scca = decompressed(*std::move(bytes));
    if (!scca) {
      continue;
    }
    const Result<apps::PrefetchFile> file = apps::parse_prefetch(*scca);
    if (!file || file->runs.empty()) {
      continue;
    }
    std::string path = apps::prefetch_executable(*file, drives);
    if (!path.empty()) {
      records.push_back(apps::RunRecord{.source = apps::UsageSource::prefetch,
                                        .path = std::move(path),
                                        .last_run = file->runs.front()});
    }
  }
  return records;
}

Result<std::vector<apps::RunRecord>> Win32AppSystem::bam() const {
  auto values = values_of(HKEY_LOCAL_MACHINE, std::wstring(bam_key) + widen(owner_sid_));
  if (!values) {
    if (values.error().code == Errc::access_denied) {
      return fail(Errc::access_denied, "BAM needs administrator rights");
    }
    return tl::unexpected(values.error());
  }
  const std::map<std::wstring, std::wstring> devices = drives_by_device();
  std::vector<apps::RunRecord> records;
  for (const auto& [name, data] : *values) {
    const std::optional<WallTime> time = apps::from_filetime(u64_of(data));
    if (data.size() < sizeof(std::uint64_t) || !time) {
      continue;
    }
    std::wstring path = name;
    const std::wstring folded = lower(name);
    for (const auto& [device, drive] : devices) {
      if (folded.starts_with(device + L"\\")) {
        path = drive + name.substr(device.size());
        break;
      }
    }
    records.push_back(
        apps::RunRecord{.source = apps::UsageSource::bam, .path = narrow(path), .last_run = *time});
  }
  return records;
}

Result<std::string> Win32AppSystem::manufacturer() const {
  const std::optional<std::wstring> maker =
      reg_string(HKEY_LOCAL_MACHINE, LR"(HARDWARE\DESCRIPTION\System\BIOS)", L"SystemManufacturer");
  if (!maker) {
    return fail(Errc::not_found, "the system manufacturer is not recorded");
  }
  return narrow(*maker);
}

}  // namespace myclean::platform
