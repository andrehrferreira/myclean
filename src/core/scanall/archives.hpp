#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/content.hpp"
#include "core/space/scanner.hpp"

// Archives already extracted (scan-all spec, "Archives Already Extracted"): .zip and .rar sets
// whose members exist unpacked in the trees. Only member lists are read (the zip central
// directory, the rar file headers); nothing is unpacked. 7z is not listed: its header is
// usually compressed, so listing it would mean decompressing.
namespace myclean::scanall {

// A file inside an archive, as its member list describes it.
struct ArchiveMember {
  // Its path inside the archive, '/' or '\' separated. UTF-8 when the archive says so (zip
  // flag bit 11, RAR5); otherwise the bytes as stored (zip CP437, the RAR4 OEM name), which
  // match unpacked names only when they are ASCII.
  std::string name;
  std::uint64_t size = 0;  // unpacked
};

// Where a zip's central directory is, relative to the file holding its end records.
struct ZipDirectory {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

// What the end of a zip says: its central directory, or where its ZIP64 end record is.
struct ZipEnd {
  std::optional<ZipDirectory> directory;
  std::optional<std::uint64_t> zip64_record;  // read 56 bytes there; then parse_zip64_end
};

inline constexpr std::size_t zip64_end_size = 56;
// The end of central directory record, its longest comment and the ZIP64 locator before it.
inline constexpr std::size_t zip_tail_size = 22 + 65535 + 20;

// The end records in `tail`, the last bytes of a zip that start at `tail_offset` in the file.
// Fails with invalid_data when there is none or the directory starts in another part of a split
// archive.
[[nodiscard]] Result<ZipEnd> parse_zip_end(std::string_view tail, std::uint64_t tail_offset);

// The central directory a ZIP64 end of central directory record names.
[[nodiscard]] Result<ZipDirectory> parse_zip64_end(std::string_view record);

// The members of a central directory (ZIP64 sizes included); folders are left out. Fails with
// invalid_data on a malformed or encrypted directory.
[[nodiscard]] Result<std::vector<ArchiveMember>> parse_zip_directory(std::string_view directory);

enum class RarVersion { rar4, rar5 };

// The version of a RAR archive starting with `head` (its first 8 bytes), and the size of its
// signature; nullopt for anything else (self-extracting archives included).
[[nodiscard]] std::optional<RarVersion> rar_version(std::string_view head);
[[nodiscard]] std::size_t rar_signature_size(RarVersion version);

// One block (header and the data after it) of a RAR archive.
struct RarBlock {
  std::uint64_t size = 0;  // the next block starts this far on
  // When not zero, the header is longer than the bytes given: parse it again with this many.
  std::size_t needed = 0;
  std::optional<ArchiveMember> member;  // a file header's member; never a folder's
  bool end = false;                     // the end of the archive or volume
};

// The block at the start of `bytes`. Fails with refused for encrypted headers (whose names
// cannot be read without the password) and with invalid_data for anything malformed.
[[nodiscard]] Result<RarBlock> parse_rar_block(std::string_view bytes, RarVersion version);

enum class ArchiveFormat { zip, rar };

struct ArchiveVolume {
  std::filesystem::path path;
  Bytes size;
  Bytes size_on_disk;
};

// A single archive or the volumes of a multi-part one, counted once.
struct ArchiveSet {
  ArchiveFormat format = ArchiveFormat::zip;
  // In volume order. A zip's first volume is X.zip, which holds the central directory, then
  // X.z01, X.z02...; a rar's X.part1.rar, X.part2.rar... or X.rar, X.r00, X.r01...
  std::vector<ArchiveVolume> volumes;
  Bytes size;  // of every volume
  Bytes size_on_disk;
};

// The archive sets among the files of `folder`: links, cloud files and further hard links are
// left out, and so is a split zip without its X.zip.
[[nodiscard]] std::vector<ArchiveSet> archive_sets(const space::Folder& folder);

struct ArchiveOptions {
  Bytes min_size{std::uint64_t{256} << 20U};  // smaller sets are not listed
  // Smaller members are not compared. Not below the files the trees still hold.
  Bytes min_member{std::uint64_t{1} << 20U};
  double min_share = 0.9;                       // of the members' bytes found unpacked
  Bytes budget{std::uint64_t{512} << 20U};      // archive bytes read, at most
  Bytes per_archive{std::uint64_t{32} << 20U};  // of one set, at most
};

struct ArchiveRun {
  std::vector<Finding> findings;
  std::size_t listed = 0;      // sets whose member lists were read
  std::size_t unreadable = 0;  // sets that could not be listed: encrypted, malformed, unreadable
  std::size_t skipped = 0;     // sets left out by the byte budgets
  Bytes read;                  // archive bytes read
  bool cancelled = false;      // stopped by `stop`
};

// The archive sets of at least min_size in `trees` (one per drive), largest first, outside the
// system_location() folders, listed through `reader` within the byte budgets. A set whose
// members of min_member or more have at least min_share of their bytes in files of `trees` with
// the same name (case-insensitive) and size, other than the set's own volumes, is a
// manual-review finding with a review plan: the archives are the owner's downloads and
// purchases. The reason gives the share and the folder holding the unpacked copy, and says when
// that copy is only inside a folder named like a backup.
[[nodiscard]] ArchiveRun find_extracted_archives(std::span<const space::ScanResult> trees,
                                                 const space::IContentReader& reader,
                                                 const ArchiveOptions& options = {},
                                                 const std::stop_token& stop = {});

}  // namespace myclean::scanall
