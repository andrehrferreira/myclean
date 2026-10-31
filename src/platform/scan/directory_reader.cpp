#include "platform/scan/directory_reader.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "platform/scan/scan_support.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using scan_support::os_error;

// Larger than any data that NTFS keeps inside the file record: a file this size or smaller may
// legitimately occupy no clusters.
constexpr std::uint64_t resident_limit = 4096;
constexpr std::size_t listing_buffer_bytes = std::size_t{128} * 1024;

WallTime wall_time(LARGE_INTEGER file_time) {
  // FILETIME counts 100 ns ticks since 1601-01-01; the Unix epoch is 11644473600 s later.
  constexpr std::int64_t epoch_ticks = 116444736000000000;
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return WallTime{
      std::chrono::duration_cast<WallTime::duration>(Ticks{file_time.QuadPart - epoch_ticks})};
}

FileIdentity identity_of(std::uint64_t volume_serial, const FILE_ID_128& id) {
  FileIdentity identity{.volume_serial = volume_serial};
  static_assert(sizeof(id.Identifier) == sizeof(identity.file_id));
  std::memcpy(identity.file_id.data(), &id.Identifier, sizeof(id.Identifier));
  return identity;
}

Result<wil::unique_hfile> open_entry(const fs::path& path, DWORD access) {
  const Result<std::wstring> native = scan_support::win32_path(path);
  if (!native) {
    return tl::unexpected(native.error());
  }
  wil::unique_hfile file{::CreateFileW(
      native->c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (!file) {
    return tl::unexpected(os_error(::GetLastError(), "cannot open", path));
  }
  return file;
}

// The allocation the file system reports for the open file itself (the WOF filter answers for
// a Windows-compressed file); the listing value when the query fails.
Bytes allocated_size(const fs::path& path, Bytes listed) {
  const Result<wil::unique_hfile> file = open_entry(path, FILE_READ_ATTRIBUTES);
  FILE_STANDARD_INFO standard{};
  if (!file || ::GetFileInformationByHandleEx(file->get(), FileStandardInfo, &standard,
                                              sizeof(standard)) == FALSE) {
    return listed;
  }
  return Bytes{static_cast<std::uint64_t>(standard.AllocationSize.QuadPart)};
}

space::ListedEntry entry_of(const FILE_ID_EXTD_DIR_INFO& info, std::wstring name,
                            std::uint64_t volume_serial) {
  const DWORD attributes = info.FileAttributes;
  const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const bool reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  space::ListedEntry entry{
      .name = fs::path{std::move(name)},
      .directory = directory,
      .attributes = {.reparse_point = reparse,
                     .recall_on_data_access =
                         (attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0,
                     .recall_on_open = (attributes & FILE_ATTRIBUTE_RECALL_ON_OPEN) != 0,
                     .offline = (attributes & FILE_ATTRIBUTE_OFFLINE) != 0},
      .reparse_tag = reparse ? info.ReparsePointTag : 0,
      .created = wall_time(info.CreationTime),
      .modified = wall_time(info.LastWriteTime),
      .identity = identity_of(volume_serial, info.FileId),
  };
  if (!directory) {
    entry.size = Bytes{static_cast<std::uint64_t>(info.EndOfFile.QuadPart)};
    entry.size_on_disk = Bytes{static_cast<std::uint64_t>(info.AllocationSize.QuadPart)};
  }
  return entry;
}

// True when the listed allocation cannot be the file's real one (see the class comment).
bool allocation_unreliable(const space::ListedEntry& entry, DWORD attributes) {
  constexpr DWORD allocation_attributes =
      FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT;
  return !entry.directory && (attributes & allocation_attributes) == 0 &&
         !scan_support::is_placeholder(attributes) && entry.size_on_disk < entry.size &&
         entry.size.count() > resident_limit;
}

}  // namespace

Result<std::vector<space::ListedEntry>> Win32DirectoryReader::list(const fs::path& path) const {
  const Result<wil::unique_hfile> directory =
      open_entry(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE);
  if (!directory) {
    return tl::unexpected(directory.error());
  }
  FILE_BASIC_INFO basic{};
  FILE_ID_INFO id{};
  if (::GetFileInformationByHandleEx(directory->get(), FileBasicInfo, &basic, sizeof(basic)) ==
          FALSE ||
      ::GetFileInformationByHandleEx(directory->get(), FileIdInfo, &id, sizeof(id)) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot query", path));
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return fail(Errc::invalid_argument, "not a directory: " + scan_support::utf8(path));
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(Errc::invalid_argument, "is a reparse point: " + scan_support::utf8(path));
  }

  std::vector<std::byte> buffer(listing_buffer_bytes);
  const std::size_t name_offset = offsetof(FILE_ID_EXTD_DIR_INFO, FileName);
  std::vector<space::ListedEntry> entries;
  FILE_INFO_BY_HANDLE_CLASS info_class = FileIdExtdDirectoryRestartInfo;
  for (;;) {
    if (::GetFileInformationByHandleEx(directory->get(), info_class, buffer.data(),
                                       static_cast<DWORD>(buffer.size())) == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
        break;
      }
      return tl::unexpected(os_error(error, "cannot list", path));
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
        space::ListedEntry entry = entry_of(info, std::move(name), id.VolumeSerialNumber);
        if (allocation_unreliable(entry, info.FileAttributes)) {
          entry.size_on_disk = allocated_size(path / entry.name, entry.size_on_disk);
        }
        entries.push_back(std::move(entry));
      }
      if (info.NextEntryOffset == 0) {
        break;
      }
      offset += info.NextEntryOffset;
    }
  }
  return entries;
}

}  // namespace myclean::platform
