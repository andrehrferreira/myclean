#include "sandbox.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which it needs.
#include <winioctl.h>

namespace myclean::platform::testing {

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

wil::unique_hfile open_for_write(const fs::path& path) {
  return wil::unique_hfile{::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                         OPEN_EXISTING, 0, nullptr)};
}

}  // namespace

Sandbox::Sandbox() {
  static std::atomic<std::uint32_t> counter{0};
  root_ = long_temp_folder() / L"myclean-it" /
          (L"compress-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++counter));
  fs::create_directories(root_);
}

Sandbox::~Sandbox() {
  // Only ever a folder this object created under %TEMP%\myclean-it.
  if (root_.parent_path().filename() == L"myclean-it" && root_.has_relative_path()) {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
}

std::string text_content(std::size_t size, std::uint32_t seed) {
  constexpr std::array<std::string_view, 16> words = {
      "alpha ", "beta ",  "gamma ",  "delta ", "epsilon ", "zeta ", "eta ",     "theta ",
      "iota ",  "kappa ", "lambda ", "mu ",    "nu ",      "xi ",   "omicron ", "pi\n"};
  std::mt19937 random{seed};
  std::string text;
  text.reserve(size + 16);
  while (text.size() < size) {
    text += words.at(random() % words.size());
    if (random() % 8 == 0) {
      text += std::to_string(random() % 100000);
    }
  }
  text.resize(size);
  return text;
}

std::string random_content(std::size_t size, std::uint32_t seed) {
  std::mt19937 random{seed};
  std::string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(random() & 0xFFU);
  }
  return bytes;
}

void write_file(const fs::path& path, std::string_view content, std::chrono::hours age) {
  fs::create_directories(path.parent_path());
  {
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  // FILETIME counts 100 ns ticks since 1601-01-01, 11644473600 s before the Unix epoch.
  const auto ticks =
      std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
          (std::chrono::system_clock::now() - age).time_since_epoch())
          .count() +
      116444736000000000;
  const FILETIME time{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF),
                      .dwHighDateTime = static_cast<DWORD>(ticks >> 32U)};
  const wil::unique_hfile handle{
      ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  if (!handle || ::SetFileTime(handle.get(), nullptr, nullptr, &time) == FALSE) {
    throw std::runtime_error("sandbox: cannot date a file");
  }
}

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::int64_t last_write(const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    return -1;
  }
  return static_cast<std::int64_t>((std::uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32U) |
                                   data.ftLastWriteTime.dwLowDateTime);
}

bool compress_ntfs(const fs::path& path) {
  const wil::unique_hfile handle = open_for_write(path);
  USHORT format = COMPRESSION_FORMAT_DEFAULT;
  DWORD returned = 0;
  return handle && ::DeviceIoControl(handle.get(), FSCTL_SET_COMPRESSION, &format, sizeof(format),
                                     nullptr, 0, &returned, nullptr) != FALSE;
}

bool mark_offline(const fs::path& path) {
  return ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_OFFLINE) != FALSE;
}

}  // namespace myclean::platform::testing
