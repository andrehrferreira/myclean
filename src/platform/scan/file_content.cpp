#include "platform/scan/file_content.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/duplicates.hpp"
#include "platform/scan/scan_support.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using scan_support::os_error;
using scan_support::utf8;

constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr std::size_t max_zone_bytes = std::size_t{64} * 1024;
constexpr std::size_t read_chunk_bytes = std::size_t{1024} * 1024;

// Reparse points whose content a file system filter serves in place: Windows compression and
// data deduplication. They name no other entry, so reading them follows nothing.
bool content_in_place(DWORD tag) {
  return tag == IO_REPARSE_TAG_WOF || tag == IO_REPARSE_TAG_DEDUP;
}

// Refuses an entry whose content must not be read: a folder, a cloud file, a link.
Result<void> check_readable(DWORD attributes, DWORD tag, const fs::path& path) {
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(Errc::invalid_argument, "not a file: " + utf8(path));
  }
  if (scan_support::is_placeholder(attributes)) {
    return fail(Errc::refused, "cloud file, not read: " + utf8(path));
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && !content_in_place(tag)) {
    return fail(Errc::refused, "reparse point, not followed: " + utf8(path));
  }
  return {};
}

// Checks the entry by its directory record, so that nothing is opened before it is known safe.
Result<void> check_entry(const std::wstring& native, const fs::path& path) {
  WIN32_FIND_DATAW data{};
  const wil::unique_hfind find{::FindFirstFileExW(native.c_str(), FindExInfoBasic, &data,
                                                  FindExSearchNameMatch, nullptr, 0)};
  if (!find) {
    return tl::unexpected(os_error(::GetLastError(), "cannot find", path));
  }
  return check_readable(data.dwFileAttributes, data.dwReserved0, path);
}

Result<wil::unique_hfile> open_read(const std::wstring& native, const fs::path& path, DWORD flags) {
  wil::unique_hfile file{::CreateFileW(native.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                       share_all, nullptr, OPEN_EXISTING, flags, nullptr)};
  if (!file) {
    return tl::unexpected(os_error(::GetLastError(), "cannot open", path));
  }
  return file;
}

Result<FILE_ID_INFO> file_id(HANDLE file, const fs::path& path) {
  FILE_ID_INFO id{};
  if (::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot query", path));
  }
  return id;
}

}  // namespace

Result<wil::unique_hfile> open_content(const fs::path& path) {
  const Result<std::wstring> native = scan_support::win32_path(path);
  if (!native) {
    return tl::unexpected(native.error());
  }
  if (Result<void> checked = check_entry(*native, path); !checked) {
    return tl::unexpected(checked.error());
  }
  Result<wil::unique_hfile> file =
      open_read(*native, path, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
  if (!file) {
    return file;
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (::GetFileInformationByHandleEx(file->get(), FileAttributeTagInfo, &tag, sizeof(tag)) ==
      FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot query", path));
  }
  if (Result<void> checked = check_readable(tag.FileAttributes, tag.ReparseTag, path); !checked) {
    return tl::unexpected(checked.error());
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
    return file;
  }
  Result<wil::unique_hfile> filtered = open_read(*native, path, FILE_FLAG_SEQUENTIAL_SCAN);
  if (!filtered) {
    return filtered;
  }
  const Result<FILE_ID_INFO> first = file_id(file->get(), path);
  const Result<FILE_ID_INFO> second = file_id(filtered->get(), path);
  if (!first || !second) {
    return tl::unexpected(first ? second.error() : first.error());
  }
  if (first->VolumeSerialNumber != second->VolumeSerialNumber ||
      std::memcmp(&first->FileId, &second->FileId, sizeof(FILE_ID_128)) != 0) {
    return fail(Errc::refused, "entry changed while opened: " + utf8(path));
  }
  return filtered;
}

Result<std::optional<std::string>> Win32ZoneIdentifierReader::read(const fs::path& file) const {
  const Result<std::wstring> native = scan_support::win32_path(file);
  if (!native) {
    return tl::unexpected(native.error());
  }
  if (Result<void> checked = check_entry(*native, file); !checked) {
    return tl::unexpected(checked.error());
  }
  const wil::unique_hfile stream{::CreateFileW((*native + L":Zone.Identifier").c_str(),
                                               FILE_READ_DATA, share_all, nullptr, OPEN_EXISTING,
                                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (!stream) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return std::optional<std::string>{};
    }
    return tl::unexpected(os_error(error, "cannot open the Zone.Identifier of", file));
  }
  std::string bytes(max_zone_bytes, '\0');
  DWORD read = 0;
  if (::ReadFile(stream.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ==
      FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot read the Zone.Identifier of", file));
  }
  bytes.resize(read);
  if (bytes.starts_with("\xFF\xFE")) {
    std::wstring wide((bytes.size() - 2) / sizeof(wchar_t), L'\0');
    std::memcpy(wide.data(), std::string_view{bytes}.substr(2).data(),
                wide.size() * sizeof(wchar_t));
    return std::optional<std::string>{scan_support::narrow(wide)};
  }
  if (bytes.starts_with("\xEF\xBB\xBF")) {
    bytes.erase(0, 3);
  }
  return std::optional<std::string>{std::move(bytes)};
}

Result<space::Digest> Win32ContentHasher::hash(const fs::path& file,
                                               std::optional<Bytes> limit) const {
  const Result<wil::unique_hfile> handle = open_content(file);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  // A background reader: let every other program's I/O go first.
  FILE_IO_PRIORITY_HINT_INFO priority{.PriorityHint = IoPriorityHintLow};
  ::SetFileInformationByHandle(handle->get(), FileIoPriorityHintInfo, &priority, sizeof(priority));

  BCRYPT_HASH_HANDLE raw_hash = nullptr;
  if (!BCRYPT_SUCCESS(
          ::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &raw_hash, nullptr, 0, nullptr, 0, 0))) {
    return fail(Errc::io_error, "cannot create a SHA-256 hash");
  }
  const auto destroy = wil::scope_exit([raw_hash] { ::BCryptDestroyHash(raw_hash); });

  std::vector<std::uint8_t> buffer(read_chunk_bytes);
  std::uint64_t remaining = limit ? limit->count() : std::numeric_limits<std::uint64_t>::max();
  while (remaining > 0) {
    const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
    DWORD read = 0;
    if (::ReadFile(handle->get(), buffer.data(), wanted, &read, nullptr) == FALSE) {
      return tl::unexpected(os_error(::GetLastError(), "cannot read", file));
    }
    if (read == 0) {
      break;
    }
    if (!BCRYPT_SUCCESS(::BCryptHashData(raw_hash, buffer.data(), read, 0))) {
      return fail(Errc::io_error, "cannot hash " + utf8(file));
    }
    remaining -= read;
  }
  space::Digest digest{};
  if (!BCRYPT_SUCCESS(
          ::BCryptFinishHash(raw_hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
    return fail(Errc::io_error, "cannot hash " + utf8(file));
  }
  return digest;
}

Result<std::string> Win32ContentReader::read(const fs::path& file, std::uint64_t offset,
                                             std::size_t length) const {
  const Result<wil::unique_hfile> handle = open_content(file);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  FILE_IO_PRIORITY_HINT_INFO priority{.PriorityHint = IoPriorityHintLow};
  ::SetFileInformationByHandle(handle->get(), FileIoPriorityHintInfo, &priority, sizeof(priority));

  const LARGE_INTEGER start{.QuadPart = static_cast<LONGLONG>(offset)};
  if (::SetFilePointerEx(handle->get(), start, nullptr, FILE_BEGIN) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot seek in", file));
  }
  std::string bytes(length, '\0');
  std::size_t done = 0;
  while (done < length) {
    const auto wanted = static_cast<DWORD>(std::min<std::size_t>(length - done, read_chunk_bytes));
    DWORD read = 0;
    if (::ReadFile(handle->get(), &bytes[done], wanted, &read, nullptr) == FALSE) {
      return tl::unexpected(os_error(::GetLastError(), "cannot read", file));
    }
    if (read == 0) {
      break;  // the end of the file
    }
    done += read;
  }
  bytes.resize(done);
  return bytes;
}

Result<fs::path> downloads_folder() {
  PWSTR path = nullptr;
  const HRESULT result =
      ::SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path);
  const auto release = wil::scope_exit([path] { ::CoTaskMemFree(path); });
  if (FAILED(result)) {
    return fail(Errc::not_found,
                "cannot locate the Downloads folder: HRESULT " + std::to_string(result));
  }
  return fs::path{path};
}

}  // namespace myclean::platform
