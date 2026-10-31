#pragma once

#include <cstdint>
#include <filesystem>

#include "core/compress/compression.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

// Win32 side of transparent compression (core/compress). Every call runs its thread in Windows
// background mode (CPU priority below "below normal", very low I/O priority) for its duration
// and sets the low I/O priority hint on the handles it opens. Nothing here follows a link or
// opens a cloud file.
namespace myclean::platform {

// Reads samples for read-only (FILE_READ_DATA) and compresses them in memory with
// RtlCompressBuffer: XPRESS Huffman (the format of WOF XPRESS) per 4/8/16/32 KiB chunk and
// LZNT1 per 64 KiB unit. Files are opened sharing read, write and delete, so no other program
// is ever kept from a file.
class Win32CompressionProbe final : public compress::ICompressionProbe {
 public:
  Win32CompressionProbe() = default;

  [[nodiscard]] Result<compress::SampleSizes> sample(const std::filesystem::path& file,
                                                     std::uint64_t offset,
                                                     std::uint64_t length) const override;
};

// WofSetFileDataLocation and FSCTL_GET/DELETE_EXTERNAL_BACKING (wofutil). compress() opens the
// file for reading and denies other writers while it works. Note: WOF decompresses a file
// whenever any program opens it for writing.
class Win32Wof final : public compress::IWof {
 public:
  Win32Wof() = default;

  [[nodiscard]] Result<compress::FileState> state(const std::filesystem::path& file) const override;
  [[nodiscard]] Result<bool> compress(const std::filesystem::path& file,
                                      compress::Algorithm algorithm,
                                      const FileIdentity& expected) override;
  [[nodiscard]] Result<void> decompress(const std::filesystem::path& file) override;
  // The first 64 *.dll files of %WINDIR%\System32: on when most are WOF- or WIM-backed.
  [[nodiscard]] Result<bool> compact_os() const override;
};

// %SystemRoot%\System32\compact.exe.
[[nodiscard]] std::filesystem::path compact_path();

// %WINDIR%, for compress::Rules::windows.
[[nodiscard]] std::filesystem::path windows_folder();

// The cluster size of the volume holding the absolute path `path`.
[[nodiscard]] Result<Bytes> cluster_size(const std::filesystem::path& path);

}  // namespace myclean::platform
