#include "sandbox.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <sddl.h>
#include <winioctl.h>
#include <wofapi.h>

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

fs::path long_temp_folder() {
  std::wstring buffer(MAX_PATH + 1, L'\0');
  DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  buffer.resize(length);
  std::wstring long_form(32768, L'\0');
  length =
      ::GetLongPathNameW(buffer.c_str(), long_form.data(), static_cast<DWORD>(long_form.size()));
  if (length == 0 || length >= long_form.size()) {
    throw std::runtime_error("sandbox: cannot resolve the temp folder");
  }
  long_form.resize(length);
  return fs::path{long_form};
}

wil::unique_hfile open_for_write(const fs::path& path, DWORD flags = 0) {
  return wil::unique_hfile{::CreateFileW(extended(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                         nullptr, OPEN_EXISTING, flags, nullptr)};
}

}  // namespace

Sandbox::Sandbox() {
  static std::atomic<std::uint32_t> counter{0};
  root_ = long_temp_folder() / L"myclean-it" /
          (L"scan-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++counter));
  fs::create_directories(root_);
}

Sandbox::~Sandbox() {
  // Only ever a folder this object created under %TEMP%\myclean-it.
  if (root_.parent_path().filename() == L"myclean-it" && root_.has_relative_path()) {
    std::error_code ignored;
    fs::remove_all(extended(root_), ignored);
  }
}

fs::path extended(const fs::path& path) {
  return fs::path{L"\\\\?\\" + path.native()};
}

void write_file(const fs::path& path, std::size_t size, char fill) {
  fs::create_directories(extended(path.parent_path()));
  std::ofstream file(extended(path), std::ios::binary);
  const std::string content(size, fill);
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void write_stream(const fs::path& path, std::wstring_view stream, std::string_view text) {
  std::ofstream file(fs::path{extended(path).native() + L":" + std::wstring(stream)},
                     std::ios::binary);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool make_junction(const fs::path& link, const fs::path& target) {
  fs::create_directories(link);
  const std::wstring substitute = L"\\??\\" + target.native();
  const std::wstring& print = target.native();
  const auto substitute_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  const auto print_bytes = static_cast<WORD>(print.size() * sizeof(wchar_t));

  // REPARSE_DATA_BUFFER with a MountPointReparseBuffer (ntifs.h is not part of the user SDK),
  // followed by the two NUL-terminated names.
  struct MountPointHeader {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
  };
  static_assert(sizeof(MountPointHeader) == 16);
  constexpr std::size_t names_header = 4 * sizeof(WORD);
  const std::size_t names = substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  const MountPointHeader header{
      .tag = IO_REPARSE_TAG_MOUNT_POINT,
      .data_length = static_cast<WORD>(names_header + names),
      .reserved = 0,
      .substitute_offset = 0,
      .substitute_length = substitute_bytes,
      .print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t)),
      .print_length = print_bytes,
  };
  std::vector<std::byte> buffer(sizeof(header) + names);
  const std::span<std::byte> bytes{buffer};
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.subspan(sizeof(header)).data(), substitute.c_str(), substitute_bytes);
  std::memcpy(bytes.subspan(sizeof(header) + header.print_offset).data(), print.c_str(),
              print_bytes);

  const wil::unique_hfile handle =
      open_for_write(link, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
  DWORD returned = 0;
  return handle && ::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), nullptr, 0, &returned,
                                     nullptr) != FALSE;
}

bool make_sparse(const fs::path& path, std::size_t size) {
  const wil::unique_hfile handle = open_for_write(path);
  DWORD returned = 0;
  const LARGE_INTEGER end{.QuadPart = static_cast<LONGLONG>(size)};
  return handle &&
         ::DeviceIoControl(handle.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                           nullptr) != FALSE &&
         ::SetFilePointerEx(handle.get(), end, nullptr, FILE_BEGIN) != FALSE &&
         ::SetEndOfFile(handle.get()) != FALSE;
}

bool compress_ntfs(const fs::path& path) {
  const wil::unique_hfile handle = open_for_write(path);
  USHORT format = COMPRESSION_FORMAT_DEFAULT;
  DWORD returned = 0;
  return handle && ::DeviceIoControl(handle.get(), FSCTL_SET_COMPRESSION, &format, sizeof(format),
                                     nullptr, 0, &returned, nullptr) != FALSE;
}

bool compress_wof(const fs::path& path) {
  const wil::unique_hfile handle = open_for_write(path);
  FILE_PROVIDER_EXTERNAL_INFO_V1 info{.Version = FILE_PROVIDER_CURRENT_VERSION,
                                      .Algorithm = FILE_PROVIDER_COMPRESSION_XPRESS4K,
                                      .Flags = 0};
  return handle &&
         SUCCEEDED(::WofSetFileDataLocation(handle.get(), WOF_PROVIDER_FILE, &info, sizeof(info)));
}

bool mark_offline(const fs::path& path) {
  return ::SetFileAttributesW(extended(path).c_str(), FILE_ATTRIBUTE_OFFLINE) != FALSE;
}

bool deny_listing(const fs::path& path, bool deny) {
  // Everyone: full access, minus listing the folder when `deny`; not inherited from the parent.
  const wchar_t* sddl = deny ? L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)" : L"D:P(A;;FA;;;WD)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                             nullptr) == FALSE) {
    return false;
  }
  const wil::unique_hlocal owner{descriptor};
  return ::SetFileSecurityW(extended(path).c_str(), DACL_SECURITY_INFORMATION, descriptor) != FALSE;
}

}  // namespace myclean::platform
