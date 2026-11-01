#include "platform/fs/win32_file_system.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

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

Errc errc_of(DWORD error) {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
      return Errc::not_found;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
      return Errc::access_denied;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return Errc::in_use;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return Errc::already_exists;
    case ERROR_DIR_NOT_EMPTY:
      return Errc::not_empty;
    case ERROR_NOT_SAME_DEVICE:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
      return Errc::invalid_argument;
    default:
      return Errc::io_error;
  }
}

tl::unexpected<Error> os_error(DWORD error, std::string_view action, const fs::path& path) {
  return fail(errc_of(error),
              std::string(action) + " " + utf8(path) + ": Win32 error " + std::to_string(error));
}

tl::unexpected<Error> last_error(std::string_view action, const fs::path& path) {
  return os_error(::GetLastError(), action, path);
}

bool is_unc(const fs::path& path) {
  return path.root_name().native().starts_with(L"\\\\");
}

// The "\\?\" form of an absolute drive or UNC path, so that MAX_PATH does not apply. Win32
// does not normalize such a path, so "." and ".." are resolved here first.
Result<std::wstring> win32_path(const fs::path& path) {
  const std::wstring& text = path.native();
  if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\") || !path.is_absolute()) {
    return fail(Errc::invalid_argument, "not an absolute drive or UNC path: " + utf8(path));
  }
  const fs::path normal = path.lexically_normal().make_preferred();
  if (is_unc(normal)) {
    return L"\\\\?\\UNC\\" + normal.native().substr(2);
  }
  return L"\\\\?\\" + normal.native();
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
    return last_error("cannot open", path);
  }
  return file;
}

os::FileStat stat_of(DWORD attributes, const FileIdentity& identity, LARGE_INTEGER end_of_file,
                     LARGE_INTEGER allocation) {
  const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  os::FileStat stat{
      .kind = directory ? os::EntryKind::directory : os::EntryKind::file,
      .attributes = {.reparse_point = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
                     .recall_on_data_access =
                         (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0,
                     .recall_on_open = (attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0,
                     .offline = (attributes & FILE_ATTRIBUTE_OFFLINE) != 0},
      .identity = identity,
  };
  if (!directory) {
    stat.size = Bytes{static_cast<std::uint64_t>(end_of_file.QuadPart)};
    stat.size_on_disk = Bytes{static_cast<std::uint64_t>(allocation.QuadPart)};
  }
  return stat;
}

FileIdentity identity_of(std::uint64_t volume_serial, const FILE_ID_128& id) {
  FileIdentity identity{.volume_serial = volume_serial};
  static_assert(sizeof(id.Identifier) == sizeof(identity.file_id));
  std::memcpy(identity.file_id.data(), &id.Identifier, sizeof(id.Identifier));
  return identity;
}

Result<os::FileStat> stat_handle(HANDLE file, const fs::path& path) {
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  if (::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ==
          FALSE ||
      ::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) == FALSE) {
    return last_error("cannot query", path);
  }
  return stat_of(basic.FileAttributes, identity_of(id.VolumeSerialNumber, id.FileId),
                 standard.EndOfFile, standard.AllocationSize);
}

// Marks the entry open in `file` (opened with DELETE) for deletion when the handle closes.
Result<void> delete_on_close(HANDLE file, const fs::path& path) {
  // POSIX semantics unlink the name at once even while another process holds the file open,
  // so that the parent folder can be removed next. The read-only attribute protects nothing
  // here (git object files carry it in every clone), so it does not stop the removal. Volumes
  // without these flags get the classic form.
  FILE_DISPOSITION_INFO_EX posix{.Flags = FILE_DISPOSITION_FLAG_DELETE |
                                          FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                          FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
  if (::SetFileInformationByHandle(file, FileDispositionInfoEx, &posix, sizeof(posix)) != FALSE) {
    return {};
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED) {
    return os_error(error, "cannot remove", path);
  }
  FILE_DISPOSITION_INFO classic{.DeleteFile = TRUE};
  if (::SetFileInformationByHandle(file, FileDispositionInfo, &classic, sizeof(classic)) == FALSE) {
    return last_error("cannot remove", path);
  }
  return {};
}

// The attributes a copy takes over from its source.
constexpr DWORD copied_attributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                    FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                                    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY;
constexpr std::size_t copy_buffer = std::size_t{1} << 20U;

// Writes the content of `source` to `target` and gives it the times and attributes of `basic`.
Result<void> copy_content(HANDLE source, HANDLE target, const fs::path& to, std::uint64_t size,
                          FILE_BASIC_INFO basic) {
  // One allocation for the whole file: less fragmentation on a hard disk.
  FILE_ALLOCATION_INFO allocation{};
  allocation.AllocationSize.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFileInformationByHandle(target, FileAllocationInfo, &allocation, sizeof(allocation)) ==
      FALSE) {
    return last_error("cannot allocate", to);
  }
  std::vector<std::byte> buffer(std::clamp<std::uint64_t>(size, 1, copy_buffer));
  for (;;) {
    DWORD read = 0;
    if (::ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
        FALSE) {
      return last_error("cannot read the source of", to);
    }
    if (read == 0) {
      break;
    }
    DWORD written = 0;
    if (::WriteFile(target, buffer.data(), read, &written, nullptr) == FALSE || written != read) {
      return last_error("cannot write", to);
    }
  }
  basic.ChangeTime.QuadPart = 0;  // left to the file system
  basic.FileAttributes &= copied_attributes;
  if (basic.FileAttributes == 0) {
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  }
  if (::SetFileInformationByHandle(target, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
    return last_error("cannot set the times of", to);
  }
  return {};
}

// FlushFileBuffers on one file. A read-only file cannot be opened for writing, so its attribute
// is lifted for the flush and put back.
Result<void> flush_file(const fs::path& path) {
  const Result<std::wstring> native = win32_path(path);
  if (!native) {
    return tl::unexpected(native.error());
  }
  const auto open = [&] {
    return wil::unique_hfile{::CreateFileW(
        native->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
  };
  wil::unique_hfile file = open();
  const DWORD attributes = ::GetFileAttributesW(native->c_str());
  const bool read_only = !file && ::GetLastError() == ERROR_ACCESS_DENIED &&
                         attributes != INVALID_FILE_ATTRIBUTES &&
                         (attributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (read_only &&
      ::SetFileAttributesW(native->c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != FALSE) {
    file = open();
    if (::SetFileAttributesW(native->c_str(), attributes) == FALSE) {
      return last_error("cannot restore the attributes of", path);
    }
  }
  if (!file) {
    return last_error("cannot open for flushing", path);
  }
  if (::FlushFileBuffers(file.get()) == FALSE) {
    return last_error("cannot flush", path);
  }
  return {};
}

// The volume device ("\\?\Volume{...}") that holds `path`; empty when it has none.
std::wstring volume_device(const fs::path& path) {
  const Result<std::wstring> native = win32_path(path);
  if (!native) {
    return {};
  }
  std::wstring mount(native->size() + 1, L'\0');
  if (::GetVolumePathNameW(native->c_str(), mount.data(), static_cast<DWORD>(mount.size())) ==
      FALSE) {
    return {};
  }
  mount.resize(std::wcslen(mount.c_str()));
  std::wstring volume(MAX_PATH, L'\0');
  if (::GetVolumeNameForVolumeMountPointW(mount.c_str(), volume.data(),
                                          static_cast<DWORD>(volume.size())) == FALSE) {
    return {};
  }
  volume.resize(std::wcslen(volume.c_str()));
  if (volume.ends_with(L'\\')) {
    volume.pop_back();  // the device, not its root folder
  }
  return volume;
}

}  // namespace

Result<os::FileStat> Win32FileSystem::stat(const fs::path& path) const {
  const Result<wil::unique_hfile> file = open_entry(path, FILE_READ_ATTRIBUTES);
  if (!file) {
    return tl::unexpected(file.error());
  }
  return stat_handle(file->get(), path);
}

Result<std::vector<os::DirEntry>> Win32FileSystem::enumerate(const fs::path& path) const {
  // The listing is read through the same handle whose attributes are checked, so the folder
  // cannot be swapped for a link between the check and the listing.
  const Result<wil::unique_hfile> directory =
      open_entry(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  if (!directory) {
    return tl::unexpected(directory.error());
  }
  const Result<os::FileStat> status = stat_handle(directory->get(), path);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->kind != os::EntryKind::directory) {
    return fail(Errc::invalid_argument, "not a directory: " + utf8(path));
  }
  if (status->attributes.reparse_point) {
    return fail(Errc::invalid_argument, "is a reparse point: " + utf8(path));
  }

  const std::size_t name_offset = offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
  std::vector<std::byte> buffer(std::size_t{64} * 1024);
  std::vector<os::DirEntry> entries;
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
        entries.push_back(
            os::DirEntry{.path = path / name,
                         .stat = stat_of(info.FileAttributes,
                                         identity_of(status->identity.volume_serial, info.FileId),
                                         info.EndOfFile, info.AllocationSize)});
      }
      if (info.NextEntryOffset == 0) {
        break;
      }
      offset += info.NextEntryOffset;
    }
  }
  return entries;
}

Result<fs::path> Win32FileSystem::expand_short_names(const fs::path& path) const {
  if (const Result<std::wstring> checked = win32_path(path); !checked) {
    return tl::unexpected(checked.error());
  }
  const fs::path normal = path.lexically_normal().make_preferred();
  const fs::path relative = normal.relative_path();
  std::wstring root = normal.root_path().native();
  if (root.size() >= 2 && root[1] == L':' && root[0] >= L'a' && root[0] <= L'z') {
    root[0] = static_cast<wchar_t>(root[0] - L'a' + L'A');
  }
  fs::path current{root};
  bool share_pending = is_unc(normal);  // the share name of a UNC path is not a directory entry
  for (const fs::path& component : relative) {
    if (component.empty()) {
      continue;
    }
    if (share_pending) {
      current /= component;
      share_pending = false;
      continue;
    }
    if (component.native().find_first_of(L"*?") != std::wstring::npos) {
      return fail(Errc::invalid_argument, "wildcard in path: " + utf8(path));
    }
    const fs::path next = current / component;
    const Result<std::wstring> native = win32_path(next);
    WIN32_FIND_DATAW data{};
    const wil::unique_hfind find{::FindFirstFileExW(native->c_str(), FindExInfoBasic, &data,
                                                    FindExSearchNameMatch, nullptr, 0)};
    if (!find) {
      return last_error("cannot find", next);
    }
    // The name as stored: long form, stored case.
    auto* const name_end = std::ranges::find(data.cFileName, L'\0');
    current /= std::wstring(std::begin(data.cFileName), name_end);
  }
  return current;
}

Result<void> Win32FileSystem::remove(const fs::path& path) {
  const Result<wil::unique_hfile> file = open_entry(path, DELETE);
  if (!file) {
    return tl::unexpected(file.error());
  }
  return delete_on_close(file->get(), path);
}

Result<void> Win32FileSystem::remove(const fs::path& path, const FileIdentity& expected) {
  // The identity is read from the handle that deletes, so no swap can slip in between.
  const Result<wil::unique_hfile> file = open_entry(path, DELETE | FILE_READ_ATTRIBUTES);
  if (!file) {
    return tl::unexpected(file.error());
  }
  const Result<os::FileStat> status = stat_handle(file->get(), path);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->identity != expected) {
    return fail(Errc::refused, "entry changed identity: " + utf8(path));
  }
  return delete_on_close(file->get(), path);
}

Result<void> Win32FileSystem::rename(const fs::path& from, const fs::path& to) {
  const Result<std::wstring> source = win32_path(from);
  const Result<std::wstring> target = win32_path(to);
  if (!source || !target) {
    return tl::unexpected(source ? target.error() : source.error());
  }
  // Without MOVEFILE_COPY_ALLOWED this is a rename on one volume; a link moves as the link.
  if (::MoveFileExW(source->c_str(), target->c_str(), 0) == FALSE) {
    return last_error("cannot rename", from);
  }
  return {};
}

Result<void> Win32FileSystem::move_file(const fs::path& from, const fs::path& to) {
  const Result<os::FileStat> status = stat(from);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (status->kind != os::EntryKind::file || status->attributes.reparse_point ||
      status->attributes.cloud_placeholder()) {
    return fail(Errc::invalid_argument, "not a regular local file: " + utf8(from));
  }
  const Result<std::wstring> source = win32_path(from);
  const Result<std::wstring> target = win32_path(to);
  if (!target) {
    return tl::unexpected(target.error());
  }
  if (::MoveFileExW(source->c_str(), target->c_str(),
                    MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) == FALSE) {
    return last_error("cannot move", from);
  }
  return {};
}

Result<os::FileStat> Win32FileSystem::copy_file(const fs::path& from, const fs::path& to,
                                                const FileIdentity& expected) {
  // The entry itself first: a placeholder is refused before anything could open its content.
  const auto check = [&](const os::FileStat& status) -> Result<void> {
    if (status.identity != expected) {
      return fail(Errc::refused, "entry changed identity: " + utf8(from));
    }
    if (status.kind != os::EntryKind::file || status.attributes.reparse_point ||
        status.attributes.cloud_placeholder()) {
      return fail(Errc::invalid_argument, "not a regular local file: " + utf8(from));
    }
    return {};
  };
  const Result<os::FileStat> entry = stat(from);
  if (!entry) {
    return tl::unexpected(entry.error());
  }
  if (Result<void> checked = check(*entry); !checked) {
    return tl::unexpected(checked.error());
  }
  const Result<std::wstring> source_name = win32_path(from);
  const Result<std::wstring> target_name = win32_path(to);
  if (!target_name) {
    return tl::unexpected(target_name.error());
  }
  // The content is read through the handle whose identity is checked, and nobody may write or
  // delete the file meanwhile.
  const wil::unique_hfile source{::CreateFileW(
      source_name->c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (!source) {
    return last_error("cannot open", from);
  }
  const Result<os::FileStat> status = stat_handle(source.get(), from);
  if (!status) {
    return tl::unexpected(status.error());
  }
  if (Result<void> checked = check(*status); !checked) {
    return tl::unexpected(checked.error());
  }
  FILE_BASIC_INFO basic{};
  if (::GetFileInformationByHandleEx(source.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
    return last_error("cannot query", from);
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0) {
    return fail(Errc::invalid_argument, "an encrypted file is not copied: " + utf8(from));
  }
  const wil::unique_hfile target{
      ::CreateFileW(target_name->c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (!target) {
    return last_error("cannot create", to);
  }
  if (Result<void> copied =
          copy_content(source.get(), target.get(), to, status->size.count(), basic);
      !copied) {
    static_cast<void>(delete_on_close(target.get(), to));
    return tl::unexpected(copied.error());
  }
  return stat_handle(target.get(), to);
}

Result<void> Win32FileSystem::flush(std::span<const fs::path> files) {
  if (files.empty()) {
    return {};
  }
  // One flush of the volume writes everything; opening it for writing needs an administrator.
  if (const std::wstring device = volume_device(files.front()); !device.empty()) {
    const wil::unique_hfile volume{::CreateFileW(device.c_str(), GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                 OPEN_EXISTING, 0, nullptr)};
    if (volume) {
      if (::FlushFileBuffers(volume.get()) == FALSE) {
        return last_error("cannot flush the volume of", files.front());
      }
      return {};
    }
  }
  for (const fs::path& file : files) {
    if (Result<void> flushed = flush_file(file); !flushed) {
      return flushed;
    }
  }
  return {};
}

Result<void> Win32FileSystem::create_directories(const fs::path& path) {
  if (const Result<std::wstring> checked = win32_path(path); !checked) {
    return tl::unexpected(checked.error());
  }
  const fs::path normal = path.lexically_normal().make_preferred();
  const fs::path relative = normal.relative_path();
  fs::path current = normal.root_path();
  bool share_pending = is_unc(normal);
  for (const fs::path& component : relative) {
    if (component.empty()) {
      continue;
    }
    current /= component;
    if (share_pending) {
      share_pending = false;
      continue;
    }
    Result<os::FileStat> status = stat(current);
    if (!status && status.error().code == Errc::not_found) {
      const Result<std::wstring> native = win32_path(current);
      if (::CreateDirectoryW(native->c_str(), nullptr) != FALSE) {
        continue;
      }
      if (::GetLastError() != ERROR_ALREADY_EXISTS) {
        return last_error("cannot create", current);
      }
      status = stat(current);  // created by someone else meanwhile
    }
    if (!status) {
      return tl::unexpected(status.error());
    }
    if (status->kind != os::EntryKind::directory || status->attributes.reparse_point) {
      return fail(Errc::invalid_argument, "not a directory or a reparse point: " + utf8(current));
    }
  }
  return {};
}

}  // namespace myclean::platform
