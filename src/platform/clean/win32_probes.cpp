#include "platform/clean/win32_probes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// clang-format off
#include <windows.h>  // first: tlhelp32.h needs its types
#include <tlhelp32.h>
// clang-format on
#include <wil/resource.h>

#include "core/clean/probes.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

tl::unexpected<Error> os_error(DWORD error, std::string_view action, const fs::path& path) {
  Errc code = Errc::io_error;
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
      error == ERROR_INVALID_DRIVE) {
    code = Errc::not_found;
  } else if (error == ERROR_ACCESS_DENIED) {
    code = Errc::access_denied;
  }
  return fail(code,
              std::string(action) + " " + utf8(path) + ": Win32 error " + std::to_string(error));
}

// The "\\?\" form of an absolute drive path, so that MAX_PATH does not apply.
Result<std::wstring> win32_path(const fs::path& path) {
  if (!path.has_root_name() || !path.has_root_directory() || path.native().starts_with(L"\\\\")) {
    return fail(Errc::invalid_argument, "not an absolute drive path: " + utf8(path));
  }
  return L"\\\\?\\" + path.lexically_normal().make_preferred().native();
}

// Opens the entry itself: a reparse point is opened as the link, never followed.
Result<wil::unique_hfile> open_entry(const fs::path& path, DWORD access) {
  const Result<std::wstring> native = win32_path(path);
  if (!native) {
    return tl::unexpected(native.error());
  }
  wil::unique_hfile file{::CreateFileW(
      native->c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (!file) {
    return os_error(::GetLastError(), "cannot open", path);
  }
  return file;
}

// A FILETIME count (100 ns since 1601-01-01 UTC) as a WallTime.
WallTime wall_time(LARGE_INTEGER value) {
  constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return WallTime{
      std::chrono::duration_cast<WallTime::duration>(Ticks{value.QuadPart - unix_epoch})};
}

clean::Entry entry_of(fs::path path, DWORD attributes, std::uint64_t volume_serial,
                      const FILE_ID_128& id, LARGE_INTEGER end_of_file, LARGE_INTEGER allocation,
                      LARGE_INTEGER created, LARGE_INTEGER written) {
  const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  clean::Entry entry{
      .path = std::move(path),
      .stat = {.kind = directory ? os::EntryKind::directory : os::EntryKind::file,
               .attributes = {.reparse_point = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
                              .recall_on_data_access =
                                  (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0,
                              .recall_on_open = (attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0,
                              .offline = (attributes & FILE_ATTRIBUTE_OFFLINE) != 0},
               .identity = {.volume_serial = volume_serial}},
      .touched = std::max(wall_time(created), wall_time(written)),
  };
  static_assert(sizeof(id.Identifier) == sizeof(entry.stat.identity.file_id));
  std::memcpy(entry.stat.identity.file_id.data(), &id.Identifier, sizeof(id.Identifier));
  if (!directory) {
    entry.stat.size = Bytes{static_cast<std::uint64_t>(end_of_file.QuadPart)};
    entry.stat.size_on_disk = Bytes{static_cast<std::uint64_t>(allocation.QuadPart)};
  }
  return entry;
}

Result<clean::Entry> stat_handle(HANDLE file, const fs::path& path) {
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  if (::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ==
          FALSE ||
      ::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) == FALSE) {
    return os_error(::GetLastError(), "cannot query", path);
  }
  return entry_of(path, basic.FileAttributes, id.VolumeSerialNumber, id.FileId, standard.EndOfFile,
                  standard.AllocationSize, basic.CreationTime, basic.LastWriteTime);
}

// The address `offset` bytes past `base` in another process, for ReadProcessMemory only.
const void* remote(std::uintptr_t base, std::uintptr_t offset) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  return reinterpret_cast<const void*>(base + offset);
}

template <typename Value>
bool read_remote(HANDLE process, std::uintptr_t base, std::uintptr_t offset, Value& value) {
  SIZE_T read = 0;
  return ::ReadProcessMemory(process, remote(base, offset), &value, sizeof(value), &read) !=
             FALSE &&
         read == sizeof(value);
}

using QueryInformationProcess = LONG(__stdcall*)(HANDLE, int, void*, ULONG, ULONG*);

// NtQueryInformationProcess, resolved from ntdll at run time like platform/proc.
QueryInformationProcess query_information_process() {
  static QueryInformationProcess function = [] {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
    return reinterpret_cast<QueryInformationProcess>(
        ntdll == nullptr ? nullptr : ::GetProcAddress(ntdll, "NtQueryInformationProcess"));
  }();
  return function;
}

// The current directory of a 64-bit process opened with PROCESS_VM_READ, from
// PEB.ProcessParameters->CurrentDirectory.DosPath (documented offsets of the x64 layouts).
std::optional<fs::path> current_directory(HANDLE process) {
  BOOL wow64 = FALSE;
  QueryInformationProcess query = query_information_process();
  if (query == nullptr || ::IsWow64Process(process, &wow64) == FALSE || wow64 != FALSE) {
    return std::nullopt;
  }
  struct BasicInformation {  // PROCESS_BASIC_INFORMATION
    LONG exit_status;
    std::uintptr_t peb;
    std::uintptr_t affinity_mask;
    LONG base_priority;
    std::uintptr_t process_id;
    std::uintptr_t parent_process_id;
  } basic{};
  constexpr int process_basic_information = 0;
  if (query(process, process_basic_information, &basic, sizeof(basic), nullptr) != 0) {
    return std::nullopt;
  }
  struct UnicodeString {  // UNICODE_STRING
    USHORT length;
    USHORT maximum_length;
    std::uintptr_t buffer;
  } dos_path{};
  constexpr std::uintptr_t process_parameters_offset = 0x20;  // in PEB
  constexpr std::uintptr_t current_directory_offset = 0x38;   // in RTL_USER_PROCESS_PARAMETERS
  std::uintptr_t parameters = 0;
  if (!read_remote(process, basic.peb, process_parameters_offset, parameters) ||
      !read_remote(process, parameters, current_directory_offset, dos_path) ||
      dos_path.length == 0 || dos_path.length % 2 != 0) {
    return std::nullopt;
  }
  std::wstring text(dos_path.length / sizeof(wchar_t), L'\0');
  SIZE_T read = 0;
  if (::ReadProcessMemory(process, remote(dos_path.buffer, 0), text.data(), dos_path.length,
                          &read) == FALSE ||
      read != dos_path.length) {
    return std::nullopt;
  }
  const fs::path folder{text};
  return folder.has_relative_path() && text.ends_with(L'\\') ? folder.parent_path() : folder;
}

}  // namespace

Result<clean::Entry> Win32FileTree::stat(const fs::path& path) const {
  const Result<wil::unique_hfile> file = open_entry(path, FILE_READ_ATTRIBUTES);
  if (!file) {
    return tl::unexpected(file.error());
  }
  return stat_handle(file->get(), path);
}

Result<std::vector<clean::Entry>> Win32FileTree::list(const fs::path& path) const {
  // The listing is read through the handle whose attributes are checked, so the folder cannot
  // be swapped for a link between the check and the listing.
  const Result<wil::unique_hfile> directory =
      open_entry(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  if (!directory) {
    return tl::unexpected(directory.error());
  }
  const Result<clean::Entry> self = stat_handle(directory->get(), path);
  if (!self) {
    return tl::unexpected(self.error());
  }
  if (self->stat.kind != os::EntryKind::directory || self->stat.attributes.reparse_point) {
    return fail(Errc::invalid_argument, "not a folder: " + utf8(path));
  }

  const std::size_t name_offset = offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
  std::vector<std::byte> buffer(std::size_t{64} * 1024);
  std::vector<clean::Entry> entries;
  FILE_INFO_BY_HANDLE_CLASS info_class = FileIdExtdDirectoryRestartInfo;
  for (;;) {
    if (::GetFileInformationByHandleEx(directory->get(), info_class, buffer.data(),
                                       static_cast<DWORD>(buffer.size())) == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
        break;
      }
      return os_error(error, "cannot list", path);
    }
    info_class = FileIdExtdDirectoryInfo;
    const std::span<const std::byte> records{buffer};
    for (std::size_t offset = 0;;) {
      FILE_ID_EXTD_DIR_INFO info{};
      std::memcpy(&info, records.subspan(offset, name_offset).data(), name_offset);
      std::wstring name(info.FileNameLength / sizeof(wchar_t), L'\0');
      std::memcpy(name.data(), records.subspan(offset + name_offset, info.FileNameLength).data(),
                  info.FileNameLength);
      if (name != L"." && name != L"..") {
        entries.push_back(entry_of(path / name, info.FileAttributes,
                                   self->stat.identity.volume_serial, info.FileId, info.EndOfFile,
                                   info.AllocationSize, info.CreationTime, info.LastWriteTime));
      }
      if (info.NextEntryOffset == 0) {
        break;
      }
      offset += info.NextEntryOffset;
    }
  }
  return entries;
}

std::vector<fs::path> Win32FileTree::fixed_volumes() const {
  std::vector<fs::path> volumes;
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const std::wstring root = {letter, L':', L'\\'};
    if ((drives & (DWORD{1} << (letter - L'A'))) != 0 &&
        ::GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
      volumes.emplace_back(root);
    }
  }
  return volumes;
}

std::optional<std::wstring> Win32Environment::variable(std::wstring_view name) const {
  const std::wstring key(name);
  std::wstring value(::GetEnvironmentVariableW(key.c_str(), nullptr, 0), L'\0');
  if (value.empty()) {
    return std::nullopt;
  }
  value.resize(
      ::GetEnvironmentVariableW(key.c_str(), value.data(), static_cast<DWORD>(value.size())));
  if (value.empty()) {
    return std::nullopt;
  }
  std::wstring long_form(::GetLongPathNameW(value.c_str(), nullptr, 0), L'\0');
  if (long_form.empty()) {
    return value;  // not an existing path
  }
  long_form.resize(
      ::GetLongPathNameW(value.c_str(), long_form.data(), static_cast<DWORD>(long_form.size())));
  return long_form.empty() ? value : long_form;
}

bool Win32Usage::locked(const fs::path& file) const {
  const Result<std::wstring> native = win32_path(file);
  if (!native) {
    return true;
  }
  const wil::unique_hfile handle{::CreateFileW(
      native->c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (handle) {
    return false;
  }
  const DWORD error = ::GetLastError();
  return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

std::vector<fs::path> Win32Usage::process_folders() const {
  std::vector<fs::path> folders;
  const wil::unique_handle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if (!snapshot) {
    return folders;
  }
  PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
  for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more != FALSE;
       more = ::Process32NextW(snapshot.get(), &entry)) {
    wil::unique_process_handle process{::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID)};
    if (!process) {
      process.reset(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
    }
    if (!process) {
      continue;
    }
    std::wstring image(32768, L'\0');
    auto size = static_cast<DWORD>(image.size());
    if (::QueryFullProcessImageNameW(process.get(), 0, image.data(), &size) != FALSE) {
      image.resize(size);
      folders.push_back(fs::path{image}.parent_path());
    }
    if (std::optional<fs::path> directory = current_directory(process.get())) {
      folders.push_back(std::move(*directory));
    }
  }
  return folders;
}

}  // namespace myclean::platform
