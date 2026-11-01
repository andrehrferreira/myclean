#include "sandbox.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
#include <winioctl.h>

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

}  // namespace

Sandbox::Sandbox() : Sandbox(long_temp_folder()) {}

Sandbox::Sandbox(const fs::path& parent) {
  static std::atomic<std::uint32_t> counter{0};
  const fs::path base = parent / L"myclean-it";
  root_ = base / (std::to_wstring(::GetCurrentProcessId()) + L"-" +
                  std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++counter));
  fs::create_directories(root_);
  // Spell the root as the file system stores it, so expected paths compare exactly.
  const wil::unique_hfile handle{::CreateFileW(root_.c_str(), FILE_READ_ATTRIBUTES,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  std::wstring stored(32768, L'\0');
  const DWORD length =
      handle ? ::GetFinalPathNameByHandleW(handle.get(), stored.data(),
                                           static_cast<DWORD>(stored.size()), FILE_NAME_NORMALIZED)
             : 0;
  if (length == 0 || length >= stored.size() || !stored.starts_with(L"\\\\?\\")) {
    throw std::runtime_error("sandbox: cannot resolve the sandbox folder");
  }
  stored.resize(length);
  root_ = fs::path{stored.substr(4)};
}

Sandbox::~Sandbox() {
  // Only ever a folder this object created under %TEMP%\myclean-it.
  if (root_.parent_path().filename() == L"myclean-it" && root_.has_relative_path()) {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
}

std::optional<fs::path> second_volume() {
  std::wstring folder(32768, L'\0');
  const DWORD length = ::GetEnvironmentVariableW(L"MYCLEAN_TEST_SECOND_VOLUME", folder.data(),
                                                 static_cast<DWORD>(folder.size()));
  if (length == 0 || length >= folder.size()) {
    return std::nullopt;
  }
  folder.resize(length);
  const auto volume_of = [](const fs::path& path) {
    std::wstring volume(MAX_PATH + 1, L'\0');
    if (::GetVolumePathNameW(path.c_str(), volume.data(), static_cast<DWORD>(volume.size())) ==
        FALSE) {
      return std::wstring();
    }
    volume.resize(std::wcslen(volume.c_str()));
    return volume;
  };
  std::error_code error;
  const std::wstring other = volume_of(folder);
  if (!fs::is_directory(folder, error) || other.empty() ||
      _wcsicmp(other.c_str(), volume_of(long_temp_folder()).c_str()) == 0) {
    return std::nullopt;
  }
  return fs::path{folder};
}

void write_file(const fs::path& path, std::size_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  const std::string content(size, 'x');
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
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

  const wil::unique_hfile handle{
      ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  DWORD returned = 0;
  return handle && ::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), nullptr, 0, &returned,
                                     nullptr) != FALSE;
}

bool entry_exists(const fs::path& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace myclean::platform
