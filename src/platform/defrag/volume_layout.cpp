#include "platform/defrag/volume_layout.hpp"

#include <algorithm>
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
#include <winioctl.h>

#include "core/drives/layout.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/defrag/win32_support.hpp"

namespace myclean::platform {

namespace {

constexpr std::size_t pointers_buffer = 64ULL * 1024;
constexpr std::size_t bitmap_buffer = 1024ULL * 1024;
constexpr std::uint64_t bits_per_byte = 8;

// The "\\?\" form of an absolute drive path, so that MAX_PATH does not apply.
std::wstring long_path(const std::filesystem::path& path) {
  const std::wstring& text = path.native();
  return text.starts_with(LR"(\\)") ? text : LR"(\\?\)" + text;
}

template <typename T>
T read_at(std::span<const std::uint8_t> buffer, std::size_t offset) {
  T value{};
  std::memcpy(&value, buffer.subspan(offset, sizeof(T)).data(), sizeof(T));
  return value;
}

}  // namespace

Result<std::vector<drives::Extent>> Win32VolumeLayout::extents(
    const std::filesystem::path& file) const {
  const wil::unique_hfile handle{
      ::CreateFileW(long_path(file).c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
  if (!handle) {
    return defrag_detail::win32_error("opening", file, ::GetLastError());
  }
  std::vector<std::uint8_t> buffer(pointers_buffer);
  const std::size_t first_extent = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents);
  std::vector<drives::Extent> extents;
  STARTING_VCN_INPUT_BUFFER input{};
  while (true) {
    DWORD returned = 0;
    const BOOL done =
        ::DeviceIoControl(handle.get(), FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
                          buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    const DWORD error = done != FALSE ? ERROR_SUCCESS : ::GetLastError();
    if (error == ERROR_HANDLE_EOF) {
      return extents;  // no clusters: a resident or empty file
    }
    if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA) {
      return defrag_detail::win32_error("reading the cluster runs of", file, error);
    }
    const std::span<const std::uint8_t> data{buffer.data(), returned};
    const auto count = read_at<DWORD>(data, offsetof(RETRIEVAL_POINTERS_BUFFER, ExtentCount));
    auto vcn = read_at<LONGLONG>(data, offsetof(RETRIEVAL_POINTERS_BUFFER, StartingVcn));
    for (DWORD index = 0; index < count; ++index) {
      const std::size_t at =
          first_extent + (static_cast<std::size_t>(index) * 2 * sizeof(LONGLONG));
      if (at + (2 * sizeof(LONGLONG)) > data.size()) {
        break;
      }
      const auto next = read_at<LONGLONG>(data, at);
      const auto lcn = read_at<LONGLONG>(data, at + sizeof(LONGLONG));
      extents.push_back(
          drives::Extent{.lcn = lcn, .clusters = static_cast<std::uint64_t>(next - vcn)});
      vcn = next;
    }
    if (error == ERROR_SUCCESS || count == 0) {
      return extents;
    }
    input.StartingVcn.QuadPart = vcn;
  }
}

Result<drives::FreeSpaceLayout> Win32VolumeLayout::free_space(
    const std::filesystem::path& root, std::chrono::milliseconds budget) const {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  DWORD sectors_per_cluster = 0;
  DWORD bytes_per_sector = 0;
  DWORD free_clusters = 0;
  DWORD total_clusters = 0;
  if (::GetDiskFreeSpaceW(root.c_str(), &sectors_per_cluster, &bytes_per_sector, &free_clusters,
                          &total_clusters) == FALSE) {
    return defrag_detail::win32_error("reading the cluster size of", root, ::GetLastError());
  }
  std::wstring device = LR"(\\.\)" + root.native();
  while (device.ends_with(L'\\')) {
    device.pop_back();
  }
  const wil::unique_hfile volume{::CreateFileW(device.c_str(), FILE_READ_DATA,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, 0, nullptr)};
  if (!volume) {
    return defrag_detail::win32_error("opening the volume", root, ::GetLastError());
  }
  drives::FreeSpaceTally tally{
      Bytes{static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector}};
  std::vector<std::uint8_t> buffer(bitmap_buffer);
  const std::size_t bits_offset = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
  STARTING_LCN_INPUT_BUFFER input{};
  while (true) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return tally.finish(false);
    }
    DWORD returned = 0;
    const BOOL done =
        ::DeviceIoControl(volume.get(), FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input),
                          buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    const DWORD error = done != FALSE ? ERROR_SUCCESS : ::GetLastError();
    if (error != ERROR_SUCCESS && error != ERROR_MORE_DATA) {
      return defrag_detail::win32_error("reading the bitmap of", root, error);
    }
    const std::span<const std::uint8_t> data{buffer.data(), returned};
    const auto start = read_at<LONGLONG>(data, offsetof(VOLUME_BITMAP_BUFFER, StartingLcn));
    const auto remaining = static_cast<std::uint64_t>(
        read_at<LONGLONG>(data, offsetof(VOLUME_BITMAP_BUFFER, BitmapSize)));
    const std::uint64_t clusters =
        std::min<std::uint64_t>(remaining, (data.size() - bits_offset) * bits_per_byte);
    tally.add(data.subspan(bits_offset), clusters);
    if (error == ERROR_SUCCESS || clusters == 0) {
      return tally.finish(true);
    }
    input.StartingLcn.QuadPart = start + static_cast<LONGLONG>(clusters);
  }
}

BackgroundPriority::BackgroundPriority()
    : active_(::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE) {}

BackgroundPriority::~BackgroundPriority() {
  if (active_) {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
  }
}

}  // namespace myclean::platform
