#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/model/result.hpp"

// USN change journal records (USN_RECORD_V2 and USN_RECORD_V3), as FSCTL_READ_USN_JOURNAL,
// FSCTL_ENUM_USN_DATA and FSCTL_READ_FILE_USN_DATA return them. Pure parsing.
namespace myclean::index {

// USN_REASON_* bits the index acts on.
namespace usn_reason {
inline constexpr std::uint32_t file_delete = 0x00000200;
inline constexpr std::uint32_t close = 0x80000000;
}  // namespace usn_reason

struct UsnRecord {
  std::uint64_t file = 0;    // file reference (record + sequence)
  std::uint64_t parent = 0;  // file reference of the folder
  std::int64_t usn = 0;
  std::uint32_t reason = 0;
  std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_*
  std::string name;              // UTF-8

  friend bool operator==(const UsnRecord&, const UsnRecord&) = default;
};

// The records packed in `bytes`, each starting at an 8-byte boundary. V4 records (byte ranges)
// are skipped; a 128-bit file id (V3) must fit in 64 bits, as on NTFS.
[[nodiscard]] Result<std::vector<UsnRecord>> parse_usn_records(std::span<const std::byte> bytes);

// The output of FSCTL_READ_USN_JOURNAL (the next USN) or FSCTL_ENUM_USN_DATA (the next file
// reference): an 8-byte value followed by records.
struct UsnBuffer {
  std::uint64_t next = 0;
  std::vector<UsnRecord> records;
};
[[nodiscard]] Result<UsnBuffer> parse_usn_buffer(std::span<const std::byte> bytes);

}  // namespace myclean::index
