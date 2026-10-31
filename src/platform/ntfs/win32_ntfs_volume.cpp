#include "platform/ntfs/win32_ntfs_volume.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winioctl.h>

#include "core/index/mft.hpp"
#include "core/index/volume.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t buffer_bytes = std::size_t{4} << 20U;
constexpr std::size_t journal_bytes = std::size_t{1} << 20U;

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

Error win32_error(DWORD error, std::string_view action, const fs::path& root) {
  Errc code = Errc::io_error;
  if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD) {
    code = Errc::access_denied;
  } else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    code = Errc::not_found;
  }
  return Error{
      .code = code,
      .message = std::string(action) + " " + utf8(root) + ": Win32 error " + std::to_string(error)};
}

// Sends one read-only FSCTL: {Win32 error or ERROR_SUCCESS, bytes returned}.
std::pair<DWORD, DWORD> control(HANDLE volume, DWORD code, std::span<const std::byte> input,
                                std::span<std::byte> output) {
  // The API takes a non-const input buffer that it only reads.
  void* in =
      input.empty()
          ? nullptr
          : const_cast<std::byte*>(input.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  DWORD returned = 0;
  const BOOL ok =
      ::DeviceIoControl(volume, code, in, static_cast<DWORD>(input.size()), output.data(),
                        static_cast<DWORD>(output.size()), &returned, nullptr);
  return {ok == FALSE ? ::GetLastError() : ERROR_SUCCESS, returned};
}

}  // namespace

bool process_elevated() {
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

Result<std::unique_ptr<Win32NtfsVolume>> Win32NtfsVolume::open(const fs::path& root) {
  const std::wstring drive = root.root_name().native();
  if (drive.size() != 2 || drive[1] != L':' || !root.has_root_directory()) {
    return fail(Errc::invalid_argument, "not a drive root: " + utf8(root));
  }
  const std::wstring root_path = drive + L"\\";
  std::array<wchar_t, MAX_PATH + 1> file_system{};
  if (::GetVolumeInformationW(root_path.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                              file_system.data(),
                              static_cast<DWORD>(file_system.size())) == FALSE) {
    return tl::unexpected(win32_error(::GetLastError(), "cannot query", root));
  }
  if (std::wstring_view{file_system.data()} != L"NTFS") {
    return fail(Errc::invalid_argument, "not an NTFS volume: " + utf8(root));
  }
  const std::wstring device = L"\\\\.\\" + drive;
  wil::unique_hfile volume{::CreateFileW(device.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, 0, nullptr)};
  if (!volume) {
    const DWORD error = ::GetLastError();
    Error opened = win32_error(error, "cannot open the volume", root);
    if (error == ERROR_ACCESS_DENIED) {
      opened.message += " (reading the $MFT needs an elevated process)";
    }
    return tl::unexpected(std::move(opened));
  }
  wil::unique_virtualalloc_ptr<std::byte> buffer{static_cast<std::byte*>(
      ::VirtualAlloc(nullptr, buffer_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))};
  if (!buffer) {
    return tl::unexpected(win32_error(::GetLastError(), "cannot allocate a read buffer for", root));
  }
  std::unique_ptr<Win32NtfsVolume> opened{
      new Win32NtfsVolume(fs::path{root_path}, std::move(volume), std::move(buffer), 0)};
  const Result<index::VolumeGeometry> geometry = opened->geometry();
  if (!geometry) {
    return tl::unexpected(geometry.error());
  }
  opened->bytes_per_record_ = geometry->bytes_per_record;
  return opened;
}

Result<index::VolumeGeometry> Win32NtfsVolume::geometry() const {
  NTFS_VOLUME_DATA_BUFFER data{};
  const auto [error, returned] = control(volume_.get(), FSCTL_GET_NTFS_VOLUME_DATA, {},
                                         std::as_writable_bytes(std::span{&data, 1}));
  if (error != ERROR_SUCCESS || returned < sizeof(data)) {
    return tl::unexpected(win32_error(error, "cannot read the NTFS data of", root_));
  }
  return index::VolumeGeometry{
      .serial = static_cast<std::uint64_t>(data.VolumeSerialNumber.QuadPart),
      .bytes_per_sector = data.BytesPerSector,
      .bytes_per_cluster = data.BytesPerCluster,
      .bytes_per_record = data.BytesPerFileRecordSegment,
      .mft_start_lcn = static_cast<std::uint64_t>(data.MftStartLcn.QuadPart),
      .mft_valid_length = static_cast<std::uint64_t>(data.MftValidDataLength.QuadPart)};
}

Result<void> Win32NtfsVolume::read(std::uint64_t offset, std::span<std::byte> into) const {
  const std::span<std::byte> buffer{buffer_.get(), buffer_bytes};
  for (std::size_t done = 0; done < into.size();) {
    const std::size_t size = std::min(buffer_bytes, into.size() - done);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset + done);
    DWORD returned = 0;
    if (::SetFilePointerEx(volume_.get(), at, nullptr, FILE_BEGIN) == FALSE ||
        ::ReadFile(volume_.get(), buffer.data(), static_cast<DWORD>(size), &returned, nullptr) ==
            FALSE) {
      return tl::unexpected(win32_error(::GetLastError(), "cannot read", root_));
    }
    if (returned != size) {
      return fail(Errc::io_error, "short read of " + utf8(root_));
    }
    std::ranges::copy(buffer.first(size), into.subspan(done).begin());
    done += size;
  }
  return {};
}

Result<std::optional<std::vector<std::byte>>> Win32NtfsVolume::file_record(
    std::uint64_t record) const {
  NTFS_FILE_RECORD_INPUT_BUFFER input{};
  input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(record);
  const std::size_t header = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
  std::vector<std::byte> output(header + bytes_per_record_);
  const auto [error, returned] = control(volume_.get(), FSCTL_GET_NTFS_FILE_RECORD,
                                         std::as_bytes(std::span{&input, 1}), std::span{output});
  if (error != ERROR_SUCCESS) {
    return tl::unexpected(win32_error(error, "cannot read a file record of", root_));
  }
  NTFS_FILE_RECORD_OUTPUT_BUFFER result{};
  std::memcpy(&result, output.data(), header);
  // A record that is not in use is answered with the nearest lower one that is.
  if (index::record_of(static_cast<std::uint64_t>(result.FileReferenceNumber.QuadPart)) != record) {
    return std::optional<std::vector<std::byte>>{};
  }
  const std::size_t length = std::min<std::size_t>(result.FileRecordLength, returned - header);
  return std::optional<std::vector<std::byte>>{
      std::vector<std::byte>(output.begin() + static_cast<std::ptrdiff_t>(header),
                             output.begin() + static_cast<std::ptrdiff_t>(header + length))};
}

Result<std::optional<index::JournalState>> Win32NtfsVolume::journal() const {
  USN_JOURNAL_DATA_V0 data{};
  const auto [error, returned] = control(volume_.get(), FSCTL_QUERY_USN_JOURNAL, {},
                                         std::as_writable_bytes(std::span{&data, 1}));
  if (error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
    return std::optional<index::JournalState>{};
  }
  if (error != ERROR_SUCCESS || returned < sizeof(data)) {
    return tl::unexpected(win32_error(error, "cannot query the change journal of", root_));
  }
  return std::optional<index::JournalState>{index::JournalState{
      .id = data.UsnJournalID, .first_usn = data.FirstUsn, .next_usn = data.NextUsn}};
}

Result<std::vector<std::byte>> Win32NtfsVolume::read_journal(std::uint64_t id,
                                                             std::int64_t from) const {
  READ_USN_JOURNAL_DATA_V0 input{.StartUsn = from,
                                 .ReasonMask = 0xFFFFFFFFU,
                                 .ReturnOnlyOnClose = FALSE,
                                 .Timeout = 0,
                                 .BytesToWaitFor = 0,
                                 .UsnJournalID = id};
  std::vector<std::byte> output(journal_bytes);
  const auto [error, returned] = control(volume_.get(), FSCTL_READ_USN_JOURNAL,
                                         std::as_bytes(std::span{&input, 1}), std::span{output});
  if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_NOT_ACTIVE ||
      error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
    return fail(Errc::not_found,
                "the change journal of " + utf8(root_) + " no longer holds the records asked for");
  }
  if (error != ERROR_SUCCESS) {
    return tl::unexpected(win32_error(error, "cannot read the change journal of", root_));
  }
  output.resize(returned);
  return output;
}

Result<std::vector<std::byte>> Win32NtfsVolume::enumerate(std::uint64_t from) const {
  MFT_ENUM_DATA_V0 input{.StartFileReferenceNumber = from, .LowUsn = 0, .HighUsn = MAXLONGLONG};
  std::vector<std::byte> output(journal_bytes);
  const auto [error, returned] = control(volume_.get(), FSCTL_ENUM_USN_DATA,
                                         std::as_bytes(std::span{&input, 1}), std::span{output});
  if (error == ERROR_HANDLE_EOF) {
    return std::vector<std::byte>{};
  }
  if (error != ERROR_SUCCESS) {
    return tl::unexpected(win32_error(error, "cannot enumerate the files of", root_));
  }
  output.resize(returned);
  return output;
}

}  // namespace myclean::platform
