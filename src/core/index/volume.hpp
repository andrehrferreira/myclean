#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "core/index/mft.hpp"
#include "core/model/result.hpp"

namespace myclean::index {

// FSCTL_GET_NTFS_VOLUME_DATA, the part the index needs.
struct VolumeGeometry {
  std::uint64_t serial = 0;  // NTFS 64-bit volume serial, as FILE_ID_INFO reports it
  std::uint32_t bytes_per_sector = 0;
  std::uint32_t bytes_per_cluster = 0;
  std::uint32_t bytes_per_record = 0;  // size of one FILE record segment
  std::uint64_t mft_start_lcn = 0;
  std::uint64_t mft_valid_length = 0;  // bytes of the $MFT that hold records
};

// FSCTL_QUERY_USN_JOURNAL: the journal's identity and the USN range it still holds.
struct JournalState {
  std::uint64_t id = 0;
  std::int64_t first_usn = 0;  // older records were overwritten
  std::int64_t next_usn = 0;   // the USN the next change gets

  friend bool operator==(const JournalState&, const JournalState&) = default;
};

// Read-only access to one NTFS volume. Implemented by src/platform/ntfs over a volume handle
// (needs elevation); tests use an in-memory volume image. Nothing here writes to the volume,
// creates or deletes a journal, or opens the indexed files.
class INtfsVolume {
 public:
  virtual ~INtfsVolume() = default;
  INtfsVolume(const INtfsVolume&) = delete;
  INtfsVolume& operator=(const INtfsVolume&) = delete;
  INtfsVolume(INtfsVolume&&) = delete;
  INtfsVolume& operator=(INtfsVolume&&) = delete;

  // The volume's root folder, such as "C:\".
  [[nodiscard]] virtual std::filesystem::path root() const = 0;
  [[nodiscard]] virtual Result<VolumeGeometry> geometry() const = 0;
  // Reads `into.size()` bytes at byte `offset` of the volume. Offset and size are multiples of
  // the sector size.
  [[nodiscard]] virtual Result<void> read(std::uint64_t offset,
                                          std::span<std::byte> into) const = 0;
  // FILE record segment `record` as NTFS returns it (FSCTL_GET_NTFS_FILE_RECORD); nullopt when
  // the record is not in use.
  [[nodiscard]] virtual Result<std::optional<std::vector<std::byte>>> file_record(
      std::uint64_t record) const = 0;
  // The change journal; nullopt when the volume has none (the index never creates one).
  [[nodiscard]] virtual Result<std::optional<JournalState>> journal() const = 0;
  // FSCTL_READ_USN_JOURNAL output from USN `from`, every reason, without waiting: the next USN
  // followed by records (none when `from` is the end). Fails with not_found when the records at
  // `from` were already overwritten or the journal `id` no longer exists.
  [[nodiscard]] virtual Result<std::vector<std::byte>> read_journal(std::uint64_t id,
                                                                    std::int64_t from) const = 0;
  // FSCTL_ENUM_USN_DATA output from file reference `from`: the next reference followed by one
  // record per file (one name each); empty after the last file.
  [[nodiscard]] virtual Result<std::vector<std::byte>> enumerate(std::uint64_t from) const = 0;

 protected:
  INtfsVolume() = default;
};

// The first `length` bytes of a non-resident attribute stored in `extents` (a sparse run
// reads as zeros).
[[nodiscard]] Result<std::vector<std::byte>> read_extents(const INtfsVolume& volume,
                                                          std::span<const Extent> extents,
                                                          std::uint64_t length);

}  // namespace myclean::index
