#include "platform/fs/win32_file_system.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
  // POSIX semantics unlink the name at once even while another process holds the file open,
  // so that the parent folder can be removed next. Volumes without them get the classic form.
  FILE_DISPOSITION_INFO_EX posix{.Flags = FILE_DISPOSITION_FLAG_DELETE |
                                          FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
  if (::SetFileInformationByHandle(file->get(), FileDispositionInfoEx, &posix, sizeof(posix)) !=
      FALSE) {
    return {};
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED) {
    return os_error(error, "cannot remove", path);
  }
  FILE_DISPOSITION_INFO classic{.DeleteFile = TRUE};
  if (::SetFileInformationByHandle(file->get(), FileDispositionInfo, &classic, sizeof(classic)) ==
      FALSE) {
    return last_error("cannot remove", path);
  }
  return {};
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
