#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Transparent compression (compression spec): the vocabulary shared by the analyzer and the
// actions, which files are never compressed, and the two interfaces src/platform/compress
// implements: a read-only probe for estimates and the WOF (Windows Overlay Filter) operations.
namespace myclean::compress {

// The compression algorithms MyClean knows. The WOF ones keep the file's content as programs see
// it and compress it in independent chunks of 4, 8, 16 or 32 KiB (LZX). LZNT1 is NTFS
// compression: estimated for information, never applied.
enum class Algorithm { xpress4k, xpress8k, xpress16k, lzx, lznt1 };

[[nodiscard]] std::string_view to_string(Algorithm algorithm);
[[nodiscard]] std::optional<Algorithm> parse_algorithm(std::string_view text);
// True for the WOF algorithms (all but lznt1).
[[nodiscard]] constexpr bool is_wof(Algorithm algorithm) {
  return algorithm != Algorithm::lznt1;
}
// The independent unit an algorithm compresses: 4, 8, 16 or 32 KiB; 4 KiB for LZNT1.
[[nodiscard]] std::uint64_t chunk_bytes(Algorithm algorithm);

// Why a file is left uncompressed.
enum class Exclusion {
  none,
  link,            // a reparse point: never followed
  placeholder,     // a cloud file: its content may not be local
  extra_link,      // a further hard link of a file already counted
  small,           // not larger than one cluster: nothing to save
  compressed,      // already smaller on disk than its size: WOF, NTFS-compressed or sparse
  media,           // a format that is compressed already (pictures, audio, video, archives)
  disk_image,      // a virtual disk (VHD, VHDX, VMDK, VDI, QCOW2)
  database,        // databases, their journals and logs: written in place
  system_file,     // drivers and the paging, swap and hibernation files
  recent,          // modified within the recent window: likely written again
  windows,         // under %WINDIR%: CompactOS covers it
  incompressible,  // of an extension the samples showed does not compress
  pack_excluded,   // named by the `exclude` of a knowledge pack's compression target
};

[[nodiscard]] std::string_view to_string(Exclusion exclusion);

// What a folder holds, which decides the recommended algorithm.
enum class Category {
  toolchain,  // compilers, SDKs and their caches (.rustup, Visual Studio, Windows Kits)
  engine,     // game engine installs (Unreal Engine, Unity editors, Godot)
  game,       // installed games (Steam, Epic, GOG, Xbox, other launchers)
  program,    // anything else under Program Files
  other,
};

[[nodiscard]] std::string_view to_string(Category category);

// The category of `folder`, from markers in its path (case-insensitive).
[[nodiscard]] Category categorize(const std::filesystem::path& folder);

// Which files are compressed: shared by the analyzer and CompressFolder.
struct Rules {
  Bytes cluster{4096};                      // the volume's cluster size
  std::chrono::days recent{30};             // files modified more recently are left alone
  std::filesystem::path windows;            // %WINDIR%; empty: no Windows folder exclusion
  std::vector<std::string> incompressible;  // extra extensions to skip, lower case with the dot
  // A knowledge pack's compression target (packs::CompressTarget::exclude): files and folders
  // below `excluded_below` whose name matches one of `excluded_names` ('*' and '?', any case)
  // are left alone.
  std::filesystem::path excluded_below;
  std::vector<std::string> excluded_names;
};

// Why `file` of folder `folder` is left uncompressed at time `now`, or Exclusion::none.
[[nodiscard]] Exclusion exclusion(const space::Folder& folder, const space::FileItem& file,
                                  const Rules& rules, WallTime now);

// True when `path` is `folder` or inside it (case-insensitive, lexical).
[[nodiscard]] bool is_within(const std::filesystem::path& path,
                             const std::filesystem::path& folder);

// Compressed sizes of one sampled range of a file, every chunk counted as WOF stores it: no
// larger than the chunk itself.
struct SampleSizes {
  Bytes read;       // bytes of the range
  Bytes xpress4k;   // XPRESS Huffman in 4 KiB chunks: what WOF XPRESS4K writes
  Bytes xpress8k;   // in 8 KiB chunks
  Bytes xpress16k;  // in 16 KiB chunks
  Bytes xpress32k;  // in 32 KiB chunks: the base of the LZX estimate (see analyzer.hpp)
  Bytes lznt1;      // LZNT1 per 64 KiB compression unit, rounded up to 4 KiB clusters
};

// Reads file content to estimate compressibility. Implemented by src/platform/compress, which
// reads at background priority and never writes, follows a link or opens a cloud file.
class ICompressionProbe {
 public:
  virtual ~ICompressionProbe() = default;
  ICompressionProbe(const ICompressionProbe&) = delete;
  ICompressionProbe& operator=(const ICompressionProbe&) = delete;
  ICompressionProbe(ICompressionProbe&&) = delete;
  ICompressionProbe& operator=(ICompressionProbe&&) = delete;

  // Compresses the `length` bytes of `file` at `offset` (fewer at its end) in memory. Fails
  // with refused for a link or a cloud file.
  [[nodiscard]] virtual Result<SampleSizes> sample(const std::filesystem::path& file,
                                                   std::uint64_t offset,
                                                   std::uint64_t length) const = 0;

 protected:
  ICompressionProbe() = default;
};

// The compression state of one file.
struct FileState {
  FileIdentity identity;
  bool reparse_point = false;  // a reparse point other than WOF compression (link, dedup)
  bool placeholder = false;    // a cloud file
  bool ntfs_compressed = false;
  bool sparse = false;
  std::optional<Algorithm> wof;  // WOF-compressed with this algorithm
  bool external = false;         // backed by WOF in any way (also WIM-backed, CompactOS)
  Bytes size;
  Bytes size_on_disk;
};

// WOF compression of single files. Implemented by src/platform/compress at background
// priority; each call acts on one file and keeps its timestamps.
class IWof {
 public:
  virtual ~IWof() = default;
  IWof(const IWof&) = delete;
  IWof& operator=(const IWof&) = delete;
  IWof(IWof&&) = delete;
  IWof& operator=(IWof&&) = delete;

  // The state of `file`, never following a link.
  [[nodiscard]] virtual Result<FileState> state(const std::filesystem::path& file) const = 0;

  // Compresses `file` with the WOF `algorithm` while it is still the entry `expected` (checked on
  // the handle that compresses). The file is opened so that no other program may write it
  // meanwhile: fails with in_use when one has it open for writing, refused when it is another
  // entry now or a link. Returns false, leaving the file as it was, when WOF finds that
  // compressing it saves nothing.
  [[nodiscard]] virtual Result<bool> compress(const std::filesystem::path& file,
                                              Algorithm algorithm,
                                              const FileIdentity& expected) = 0;

  // Removes the WOF compression of `file` (FSCTL_DELETE_EXTERNAL_BACKING): the content is
  // written back uncompressed, byte for byte.
  [[nodiscard]] virtual Result<void> decompress(const std::filesystem::path& file) = 0;

  // True when Windows' own files are WOF-compressed (CompactOS on), judged from a sample of
  // System32 libraries so that it does not depend on the display language.
  [[nodiscard]] virtual Result<bool> compact_os() const = 0;

 protected:
  IWof() = default;
};

}  // namespace myclean::compress
