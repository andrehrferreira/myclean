#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/index/mft.hpp"
#include "core/index/volume.hpp"
#include "core/model/result.hpp"

// Test fixtures built from the documented NTFS structures (FILE_RECORD_SEGMENT_HEADER,
// ATTRIBUTE_RECORD_HEADER, STANDARD_INFORMATION, FILE_NAME, ATTRIBUTE_LIST_ENTRY, mapping
// pairs, USN_RECORD_V2/V3), and an in-memory volume that serves them like the platform does.
namespace myclean::index::testing {

inline constexpr std::uint32_t record_size = 1024;
inline constexpr std::uint32_t cluster_size = 4096;
inline constexpr std::uint64_t serial = 0x1122334455667788ULL;
// 2026-10-01 00:00:00 UTC as a FILETIME, and the same as Unix seconds.
inline constexpr std::uint64_t file_time = 134352864000000000ULL;
inline constexpr std::uint32_t unix_time = 1790812800U;

inline constexpr std::uint32_t archive = 0x20;
inline constexpr std::uint32_t tag_mount_point = 0xA0000003U;
inline constexpr std::uint32_t tag_symlink = 0xA000000CU;
inline constexpr std::uint32_t tag_wof = 0x80000017U;
inline constexpr std::uint32_t tag_cloud = 0x9000001AU;
inline constexpr std::uint16_t compressed = 0x0001;
inline constexpr std::uint16_t sparse = 0x8000;

inline constexpr std::uint8_t posix = 0;
inline constexpr std::uint8_t win32 = 1;
inline constexpr std::uint8_t dos = 2;

// The file reference of `record` with `sequence`.
[[nodiscard]] constexpr std::uint64_t ref(std::uint64_t record, std::uint16_t sequence = 1) {
  return reference_of(record, sequence);
}
inline constexpr std::uint64_t root_ref = reference_of(root_record, root_record);

// Mapping pairs for `extents` (VCNs follow each other from the first), with the terminator.
[[nodiscard]] std::vector<std::byte> encode_runs(std::span<const Extent> extents);

// One FILE record segment, attribute by attribute.
class RecordBuilder {
 public:
  explicit RecordBuilder(std::uint64_t record, std::uint16_t sequence = 1)
      : record_(record), sequence_(sequence) {}

  RecordBuilder& directory();
  RecordBuilder& not_in_use();
  RecordBuilder& base(std::uint64_t reference);
  RecordBuilder& standard_information(std::uint32_t attributes, std::uint64_t modified = file_time);
  RecordBuilder& file_name(std::uint64_t parent, std::u16string_view name,
                           std::uint8_t name_space = win32, std::uint32_t reparse_tag = 0);
  RecordBuilder& resident_data(std::u16string_view stream, std::uint64_t size);
  RecordBuilder& data(std::u16string_view stream, std::uint64_t size, std::uint64_t allocated,
                      std::vector<Extent> runs = {}, std::uint16_t flags = 0,
                      std::optional<std::uint64_t> total_allocated = std::nullopt);
  // A later part of a non-resident stream (lowest VCN above 0), as an extension segment has it.
  RecordBuilder& data_part(std::uint64_t lowest_vcn, std::vector<Extent> runs);
  RecordBuilder& reparse_point(std::uint32_t tag);
  RecordBuilder& attribute_list(std::span<const std::byte> value);
  RecordBuilder& attribute_list(std::vector<Extent> runs, std::uint64_t length);

  // As the disk holds it: every 512-byte stride ends with the update sequence number.
  [[nodiscard]] std::vector<std::byte> on_disk(std::uint16_t usn = 0x0042) const;
  // As FSCTL_GET_NTFS_FILE_RECORD returns it: the original bytes restored.
  [[nodiscard]] std::vector<std::byte> restored() const;

 private:
  [[nodiscard]] std::vector<std::byte> build(std::optional<std::uint16_t> usn) const;

  std::uint64_t record_;
  std::uint16_t sequence_;
  std::uint64_t base_ = 0;
  bool in_use_ = true;
  bool directory_ = false;
  std::vector<std::vector<std::byte>> attributes_;
};

// One ATTRIBUTE_LIST_ENTRY.
[[nodiscard]] std::vector<std::byte> list_entry(std::uint32_t type, std::uint64_t segment,
                                                std::uint64_t lowest_vcn = 0);

// USN_RECORD_V2 / V3, padded to 8 bytes.
[[nodiscard]] std::vector<std::byte> usn_v2(std::uint64_t file, std::uint64_t parent,
                                            std::int64_t usn, std::uint32_t reason,
                                            std::uint32_t attributes, std::u16string_view name);
[[nodiscard]] std::vector<std::byte> usn_v3(std::uint64_t file, std::uint64_t parent,
                                            std::int64_t usn, std::uint32_t reason,
                                            std::uint32_t attributes, std::u16string_view name);

// An NTFS volume in memory: a $MFT of `capacity` records in two runs far apart (record 0 is
// written by the volume itself, named $MFT), a change journal, and the FSCTL answers.
class FakeVolume final : public INtfsVolume {
 public:
  explicit FakeVolume(std::uint64_t capacity = 64);

  // Writes a record into the $MFT (and the answer of file_record()).
  void put(std::uint64_t record, const RecordBuilder& builder);
  // Marks a record as not in use.
  void free(std::uint64_t record);
  // Adds a journal record with the next USN.
  void journal_add(std::uint64_t file, std::uint64_t parent, std::uint32_t reason,
                   std::u16string_view name);

  // What journal() answers; reset() deletes the journal.
  [[nodiscard]] std::optional<JournalState>& journal_state() { return journal_state_; }
  // From now on read_journal fails with not_found, as when records are overwritten meanwhile.
  void overwrite_while_reading() { overwrite_while_reading_ = true; }

  [[nodiscard]] std::filesystem::path root() const override { return "C:\\"; }
  [[nodiscard]] Result<VolumeGeometry> geometry() const override;
  [[nodiscard]] Result<void> read(std::uint64_t offset, std::span<std::byte> into) const override;
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>> file_record(
      std::uint64_t record) const override;
  [[nodiscard]] Result<std::optional<JournalState>> journal() const override {
    return journal_state_;
  }
  [[nodiscard]] Result<std::vector<std::byte>> read_journal(std::uint64_t id,
                                                            std::int64_t from) const override;
  [[nodiscard]] Result<std::vector<std::byte>> enumerate(std::uint64_t from) const override;

  [[nodiscard]] const std::vector<Extent>& mft_runs() const { return runs_; }

 private:
  [[nodiscard]] std::uint64_t disk_offset(std::uint64_t record) const;

  static constexpr std::size_t journal_records_per_read = 2;

  std::uint64_t capacity_;
  std::optional<JournalState> journal_state_ = JournalState{.id = 9};
  bool overwrite_while_reading_ = false;
  std::vector<Extent> runs_;
  std::vector<std::byte> disk_;
  std::map<std::uint64_t, std::vector<std::byte>> restored_;  // in-use records
  struct Listed {
    std::uint64_t reference = 0;
    std::uint64_t parent = 0;
    std::u16string name;  // ASCII names only
  };
  std::map<std::uint64_t, Listed> listed_;                  // for enumerate()
  std::map<std::int64_t, std::vector<std::byte>> journal_;  // by USN
};

}  // namespace myclean::index::testing
