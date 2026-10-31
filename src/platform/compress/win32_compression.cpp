#include "platform/compress/win32_compression.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>
// Included after windows.h, which they need.
#include <winioctl.h>
#include <wofapi.h>

#include "core/compress/compression.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "platform/scan/scan_support.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;
using scan_support::os_error;
using scan_support::utf8;

constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr std::size_t kib = 1024;
constexpr std::size_t lznt1_unit = 64 * kib;  // NTFS compression unit with 4 KiB clusters
constexpr std::size_t ntfs_cluster = 4 * kib;
constexpr std::size_t compact_os_sample = 64;
constexpr USHORT xpress_huffman = COMPRESSION_FORMAT_XPRESS_HUFF | COMPRESSION_ENGINE_STANDARD;
constexpr USHORT lznt1 = COMPRESSION_FORMAT_LZNT1 | COMPRESSION_ENGINE_STANDARD;

// RtlCompressBuffer and RtlGetCompressionWorkSpaceSize: ntdll exports declared in the driver
// kit only, so resolved at run time like NtQueryInformationProcess in platform/proc.
using CompressBuffer = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
using WorkSpaceSize = LONG(NTAPI*)(USHORT, PULONG, PULONG);

struct Rtl {
  CompressBuffer compress = nullptr;
  WorkSpaceSize workspace = nullptr;
};

template <typename Function>
Function export_of(HMODULE module, const char* name) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress returns FARPROC
  return reinterpret_cast<Function>(::GetProcAddress(module, name));
}

const Rtl& rtl() {
  static const Rtl functions = [] {
    Rtl found;
    if (const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
      found.compress = export_of<CompressBuffer>(ntdll, "RtlCompressBuffer");
      found.workspace = export_of<WorkSpaceSize>(ntdll, "RtlGetCompressionWorkSpaceSize");
    }
    return found;
  }();
  return functions;
}

// Windows background mode for the calling thread while it lives: CPU priority below "below
// normal", very low I/O and low memory priority. A thread already in it is left as it is.
class Background {
 public:
  Background()
      : began_(::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE) {}
  ~Background() {
    if (began_) {
      ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
  }
  Background(const Background&) = delete;
  Background& operator=(const Background&) = delete;
  Background(Background&&) = delete;
  Background& operator=(Background&&) = delete;

 private:
  bool began_;
};

void low_io_priority(HANDLE file) {
  FILE_IO_PRIORITY_HINT_INFO priority{.PriorityHint = IoPriorityHintLow};
  ::SetFileInformationByHandle(file, FileIoPriorityHintInfo, &priority, sizeof(priority));
}

// Opens `path` itself, never a link's target.
Result<wil::unique_hfile> open(const fs::path& path, DWORD access, DWORD share) {
  const Result<std::wstring> native = scan_support::win32_path(path);
  if (!native) {
    return tl::unexpected(native.error());
  }
  wil::unique_hfile file{::CreateFileW(native->c_str(), access, share, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (!file) {
    return tl::unexpected(os_error(::GetLastError(), "cannot open", path));
  }
  return file;
}

std::optional<compress::Algorithm> algorithm_of(ULONG algorithm) {
  switch (algorithm) {
    case FILE_PROVIDER_COMPRESSION_XPRESS4K:
      return compress::Algorithm::xpress4k;
    case FILE_PROVIDER_COMPRESSION_XPRESS8K:
      return compress::Algorithm::xpress8k;
    case FILE_PROVIDER_COMPRESSION_XPRESS16K:
      return compress::Algorithm::xpress16k;
    case FILE_PROVIDER_COMPRESSION_LZX:
      return compress::Algorithm::lzx;
    default:
      return std::nullopt;
  }
}

ULONG wof_algorithm(compress::Algorithm algorithm) {
  switch (algorithm) {
    case compress::Algorithm::xpress8k:
      return FILE_PROVIDER_COMPRESSION_XPRESS8K;
    case compress::Algorithm::xpress16k:
      return FILE_PROVIDER_COMPRESSION_XPRESS16K;
    case compress::Algorithm::lzx:
      return FILE_PROVIDER_COMPRESSION_LZX;
    case compress::Algorithm::xpress4k:
    case compress::Algorithm::lznt1:
      break;
  }
  return FILE_PROVIDER_COMPRESSION_XPRESS4K;
}

Result<compress::FileState> state_of(HANDLE file, const fs::path& path) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE ||
      ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ==
          FALSE ||
      ::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot query", path));
  }
  const DWORD attributes = tag.FileAttributes;
  compress::FileState state{
      .identity = {.volume_serial = id.VolumeSerialNumber},
      .reparse_point =
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && tag.ReparseTag != IO_REPARSE_TAG_WOF,
      .placeholder = scan_support::is_placeholder(attributes),
      .ntfs_compressed = (attributes & FILE_ATTRIBUTE_COMPRESSED) != 0,
      .sparse = (attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
      .size = Bytes{static_cast<std::uint64_t>(standard.EndOfFile.QuadPart)},
      .size_on_disk = Bytes{static_cast<std::uint64_t>(standard.AllocationSize.QuadPart)},
  };
  static_assert(sizeof(id.FileId.Identifier) == sizeof(state.identity.file_id));
  std::memcpy(state.identity.file_id.data(), &id.FileId.Identifier, sizeof(id.FileId.Identifier));
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(Errc::invalid_argument, "not a file: " + utf8(path));
  }

  // A WIM backing (WIMBoot) is longer than a WOF one; the buffer holds either.
  std::array<std::byte, 256> backing{};
  DWORD returned = 0;
  if (::DeviceIoControl(file, FSCTL_GET_EXTERNAL_BACKING, nullptr, 0, backing.data(),
                        static_cast<DWORD>(backing.size()), &returned, nullptr) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_OBJECT_NOT_EXTERNALLY_BACKED) {
      return tl::unexpected(os_error(error, "cannot query the compression of", path));
    }
    return state;
  }
  state.external = true;
  WOF_EXTERNAL_INFO external{};
  std::memcpy(&external, backing.data(), sizeof(external));
  if (external.Provider == WOF_PROVIDER_FILE) {
    FILE_PROVIDER_EXTERNAL_INFO_V1 provider{};
    std::memcpy(&provider, std::span{backing}.subspan(sizeof(external)).data(), sizeof(provider));
    state.wof = algorithm_of(provider.Algorithm);
  }
  return state;
}

// The bytes of `data` compressed with `format` in independent pieces of `piece` bytes, each
// rounded up to `round` and counted at most at its own size (stored uncompressed otherwise).
// RtlCompressBuffer only reads `data`, though its parameter is not const.
std::uint64_t compressed_size(std::span<std::uint8_t> data, USHORT format, std::size_t piece,
                              std::size_t round, std::vector<std::uint8_t>& workspace,
                              std::vector<std::uint8_t>& output) {
  std::uint64_t total = 0;
  for (std::size_t offset = 0; offset < data.size(); offset += piece) {
    const std::span<std::uint8_t> part =
        data.subspan(offset, std::min(piece, data.size() - offset));
    ULONG written = 0;
    const LONG status =
        rtl().compress(format, part.data(), static_cast<ULONG>(part.size()), output.data(),
                       static_cast<ULONG>(output.size()), static_cast<ULONG>(ntfs_cluster),
                       &written, workspace.data());
    std::size_t stored = part.size();
    if (status >= 0) {  // NT_SUCCESS, also STATUS_BUFFER_ALL_ZEROS
      stored =
          std::min(part.size(), (static_cast<std::size_t>(written) + round - 1) / round * round);
    }
    total += stored;
  }
  return total;
}

}  // namespace

Result<compress::SampleSizes> Win32CompressionProbe::sample(const fs::path& file,
                                                            std::uint64_t offset,
                                                            std::uint64_t length) const {
  if (rtl().compress == nullptr || rtl().workspace == nullptr) {
    return fail(Errc::not_found, "RtlCompressBuffer is not exported by ntdll.dll");
  }
  const Background background;
  const Result<wil::unique_hfile> handle =
      open(file, FILE_READ_DATA | FILE_READ_ATTRIBUTES, share_all);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (::GetFileInformationByHandleEx(handle->get(), FileAttributeTagInfo, &tag, sizeof(tag)) ==
      FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot query", file));
  }
  if (scan_support::is_placeholder(tag.FileAttributes)) {
    return fail(Errc::refused, "cloud file, not read: " + utf8(file));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      tag.ReparseTag != IO_REPARSE_TAG_WOF) {
    return fail(Errc::refused, "reparse point, not followed: " + utf8(file));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(Errc::invalid_argument, "not a file: " + utf8(file));
  }
  low_io_priority(handle->get());

  std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
  const LARGE_INTEGER start{.QuadPart = static_cast<LONGLONG>(offset)};
  if (::SetFilePointerEx(handle->get(), start, nullptr, FILE_BEGIN) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot seek in", file));
  }
  std::size_t filled = 0;
  while (filled < data.size()) {
    DWORD read = 0;
    if (::ReadFile(handle->get(), std::span{data}.subspan(filled).data(),
                   static_cast<DWORD>(data.size() - filled), &read, nullptr) == FALSE) {
      return tl::unexpected(os_error(::GetLastError(), "cannot read", file));
    }
    if (read == 0) {
      break;
    }
    filled += read;
  }
  data.resize(filled);

  ULONG xpress_workspace = 0;
  ULONG lznt1_workspace = 0;
  ULONG fragment = 0;
  if (rtl().workspace(xpress_huffman, &xpress_workspace, &fragment) < 0 ||
      rtl().workspace(lznt1, &lznt1_workspace, &fragment) < 0) {
    return fail(Errc::io_error, "RtlGetCompressionWorkSpaceSize failed");
  }
  std::vector<std::uint8_t> workspace(std::max(xpress_workspace, lznt1_workspace));
  std::vector<std::uint8_t> output((2 * lznt1_unit) + ntfs_cluster);
  const std::span<std::uint8_t> content{data};
  const auto xpress = [&](std::size_t chunk) {
    return Bytes{compressed_size(content, xpress_huffman, chunk, 1, workspace, output)};
  };
  return compress::SampleSizes{
      .read = Bytes{filled},
      .xpress4k = xpress(4 * kib),
      .xpress8k = xpress(8 * kib),
      .xpress16k = xpress(16 * kib),
      .xpress32k = xpress(32 * kib),
      .lznt1 = Bytes{compressed_size(content, lznt1, lznt1_unit, ntfs_cluster, workspace, output)},
  };
}

Result<compress::FileState> Win32Wof::state(const fs::path& file) const {
  const Result<wil::unique_hfile> handle = open(file, FILE_READ_ATTRIBUTES, share_all);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  return state_of(handle->get(), file);
}

Result<bool> Win32Wof::compress(const fs::path& file, compress::Algorithm algorithm,
                                const FileIdentity& expected) {
  if (!compress::is_wof(algorithm)) {
    return fail(Errc::invalid_argument, "not a WOF algorithm: lznt1");
  }
  const Background background;
  // Sharing read only: no other program can write the file while WOF compresses it.
  const Result<wil::unique_hfile> handle = open(file, GENERIC_READ, FILE_SHARE_READ);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  const Result<compress::FileState> state = state_of(handle->get(), file);
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (state->identity != expected) {
    return fail(Errc::refused, "another file now: " + utf8(file));
  }
  if (state->reparse_point || state->placeholder || state->external || state->ntfs_compressed ||
      state->sparse) {
    return fail(Errc::refused, "not a plain uncompressed file: " + utf8(file));
  }
  low_io_priority(handle->get());
  WOF_FILE_COMPRESSION_INFO_V1 info{.Algorithm = wof_algorithm(algorithm), .Flags = 0};
  const HRESULT result =
      ::WofSetFileDataLocation(handle->get(), WOF_PROVIDER_FILE, &info, sizeof(info));
  if (result == HRESULT_FROM_WIN32(ERROR_COMPRESSION_NOT_BENEFICIAL)) {
    return false;
  }
  if (FAILED(result)) {
    return tl::unexpected(os_error(HRESULT_FACILITY(result) == FACILITY_WIN32
                                       ? HRESULT_CODE(result)
                                       : static_cast<DWORD>(result),
                                   "cannot compress", file));
  }
  return true;
}

Result<void> Win32Wof::decompress(const fs::path& file) {
  const Background background;
  const Result<wil::unique_hfile> handle = open(file, GENERIC_READ, FILE_SHARE_READ);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  const Result<compress::FileState> state = state_of(handle->get(), file);
  if (!state) {
    return tl::unexpected(state.error());
  }
  if (state->reparse_point) {
    return fail(Errc::refused, "reparse point, not followed: " + utf8(file));
  }
  if (!state->external) {
    return {};
  }
  low_io_priority(handle->get());
  DWORD returned = 0;
  if (::DeviceIoControl(handle->get(), FSCTL_DELETE_EXTERNAL_BACKING, nullptr, 0, nullptr, 0,
                        &returned, nullptr) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_OBJECT_NOT_EXTERNALLY_BACKED) {
      return tl::unexpected(os_error(error, "cannot decompress", file));
    }
  }
  return {};
}

Result<bool> Win32Wof::compact_os() const {
  const fs::path system = windows_folder() / L"System32";
  WIN32_FIND_DATAW found{};
  const wil::unique_hfind find{::FindFirstFileExW((system / L"*.dll").c_str(), FindExInfoBasic,
                                                  &found, FindExSearchNameMatch, nullptr, 0)};
  if (!find) {
    return tl::unexpected(os_error(::GetLastError(), "cannot list", system));
  }
  std::size_t checked = 0;
  std::size_t backed = 0;
  for (bool more = true; more && checked < compact_os_sample;
       more = ::FindNextFileW(find.get(), &found) != FALSE) {
    if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      continue;
    }
    const Result<compress::FileState> state = Win32Wof::state(system / found.cFileName);
    if (state) {
      ++checked;
      backed += state->external ? 1 : 0;
    }
  }
  if (checked == 0) {
    return fail(Errc::not_found, "no system library could be checked in " + utf8(system));
  }
  return 2 * backed > checked;
}

fs::path compact_path() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const UINT length = ::GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  return fs::path{std::wstring_view(buffer.data(), length)} / L"compact.exe";
}

fs::path windows_folder() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const UINT length = ::GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  return fs::path{std::wstring_view(buffer.data(), length)};
}

Result<Bytes> cluster_size(const fs::path& path) {
  std::array<wchar_t, MAX_PATH + 1> volume{};
  if (::GetVolumePathNameW(path.c_str(), volume.data(), static_cast<DWORD>(volume.size())) ==
      FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot find the volume of", path));
  }
  DWORD sectors = 0;
  DWORD bytes = 0;
  DWORD free_clusters = 0;
  DWORD clusters = 0;
  if (::GetDiskFreeSpaceW(volume.data(), &sectors, &bytes, &free_clusters, &clusters) == FALSE) {
    return tl::unexpected(os_error(::GetLastError(), "cannot read the cluster size of", path));
  }
  return Bytes{std::uint64_t{sectors} * bytes};
}

}  // namespace myclean::platform
