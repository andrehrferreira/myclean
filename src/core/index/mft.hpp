#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/model/result.hpp"

// The NTFS on-disk structures the index reads: FILE record segments of the $MFT, their
// attributes, mapping pairs (runs) and attribute lists. Layouts follow the documented NTFS
// structures (FILE_RECORD_SEGMENT_HEADER, ATTRIBUTE_RECORD_HEADER, FILE_NAME,
// STANDARD_INFORMATION, ATTRIBUTE_LIST_ENTRY). Pure parsing: nothing here touches a volume.
namespace myclean::index {

// A file reference: the record number in the low 48 bits, the sequence number in the high 16.
[[nodiscard]] constexpr std::uint64_t record_of(std::uint64_t reference) {
  return reference & 0x0000'FFFF'FFFF'FFFFULL;
}
[[nodiscard]] constexpr std::uint16_t sequence_of(std::uint64_t reference) {
  return static_cast<std::uint16_t>(reference >> 48U);
}
[[nodiscard]] constexpr std::uint64_t reference_of(std::uint64_t record, std::uint16_t sequence) {
  return record_of(record) | (std::uint64_t{sequence} << 48U);
}

// The root folder of every NTFS volume.
inline constexpr std::uint64_t root_record = 5;

// File attribute bits (FILE_ATTRIBUTE_*) as $STANDARD_INFORMATION and USN records store them.
namespace file_attribute {
inline constexpr std::uint32_t directory = 0x10;
inline constexpr std::uint32_t reparse_point = 0x400;
inline constexpr std::uint32_t offline = 0x1000;
inline constexpr std::uint32_t recall_on_open = 0x40000;
inline constexpr std::uint32_t recall_on_data_access = 0x400000;
}  // namespace file_attribute

// IsReparseTagNameSurrogate: the tag names another entry (junction, symbolic link, mount point).
inline constexpr std::uint32_t name_surrogate_bit = 0x20000000U;

// One $FILE_NAME of a file: a hard link. DOS-only (8.3) names are not reported.
struct FileName {
  std::uint64_t parent = 0;  // file reference of the folder
  std::string name;          // UTF-8
};

// A run of clusters of a non-resident attribute; `lcn` is empty for a sparse run.
struct Extent {
  std::uint64_t vcn = 0;
  std::optional<std::uint64_t> lcn;
  std::uint64_t clusters = 0;

  friend bool operator==(const Extent&, const Extent&) = default;
};

// What one FILE record segment holds that the index uses. An extension segment (`base` set)
// carries attributes of its base record that did not fit there.
struct RecordSegment {
  std::uint64_t record = 0;  // header field; callers that know the position set it
  std::uint16_t sequence = 0;
  std::uint64_t base = 0;  // file reference of the base record; 0 for a base record
  bool in_use = false;
  bool directory = false;
  bool has_standard_information = false;
  std::uint32_t attributes = 0;       // $STANDARD_INFORMATION file attributes
  std::uint64_t created = 0;          // FILETIME, $STANDARD_INFORMATION
  std::uint64_t modified = 0;         // FILETIME of the last write
  std::uint32_t reparse_tag = 0;      // resident $REPARSE_POINT, else the $FILE_NAME copy
  std::vector<FileName> names;        // $FILE_NAME attributes except DOS-only names
  std::optional<std::uint64_t> size;  // logical size of the unnamed $DATA, from its first part
  // Allocation of every $DATA stream (unnamed and named) whose first part (lowest VCN 0) is in
  // this segment: the clusters actually allocated for a compressed or sparse stream, nothing
  // for a resident one. A Windows-compressed (WOF) file counts its WofCompressedData stream.
  std::uint64_t size_on_disk = 0;
  // Runs of the unnamed $DATA parts in this segment; filled only when asked (for the $MFT).
  std::vector<Extent> data_extents;
  // $ATTRIBUTE_LIST: its value when resident, or its runs and length when not.
  bool has_attribute_list = false;
  std::vector<std::byte> attribute_list;
  std::vector<Extent> attribute_list_extents;
  std::uint64_t attribute_list_length = 0;
};

// Parses one FILE record segment in place. The update sequence (the "fixups" that protect each
// 512-byte stride) is restored when the record comes straight from the disk and accepted as is
// when it was already restored (FSCTL_GET_NTFS_FILE_RECORD). A never-used record (all zero) is
// returned as not in use. Fails with invalid_data on a damaged or torn record. `out` is reused
// so that a whole-$MFT pass does not allocate per record; `with_extents` decodes the runs of
// the unnamed $DATA.
[[nodiscard]] Result<void> parse_record(std::span<std::byte> bytes, RecordSegment& out,
                                        bool with_extents = false);

// Decodes mapping pairs (the runs of a non-resident attribute) starting at `first_vcn`.
[[nodiscard]] Result<std::vector<Extent>> decode_runs(std::span<const std::byte> pairs,
                                                      std::uint64_t first_vcn);

// The record numbers other than `base_record` that an $ATTRIBUTE_LIST value names: the
// extension segments to read, in order of first appearance.
[[nodiscard]] Result<std::vector<std::uint64_t>> extension_records(
    std::span<const std::byte> attribute_list, std::uint64_t base_record);

// UTF-8 of the UTF-16LE text in `bytes`; an unpaired surrogate becomes
// U+FFFD.
[[nodiscard]] std::string utf8_of_utf16(std::span<const std::byte> bytes);

}  // namespace myclean::index
