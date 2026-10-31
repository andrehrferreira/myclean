#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <wil/resource.h>

#include "core/index/volume.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// True when the process token is elevated, which opening a volume for the index needs.
[[nodiscard]] bool process_elevated();

// index::INtfsVolume over a volume handle (\\.\C:) opened for reading. It sends only read
// requests: ReadFile of the $MFT clusters, FSCTL_GET_NTFS_VOLUME_DATA,
// FSCTL_GET_NTFS_FILE_RECORD, FSCTL_QUERY_USN_JOURNAL, FSCTL_READ_USN_JOURNAL and
// FSCTL_ENUM_USN_DATA. It never creates or deletes a journal and never opens the indexed files.
// Volume reads are non-cached, so they go through a page-aligned 4 MiB buffer; not
// thread-safe.
class Win32NtfsVolume final : public index::INtfsVolume {
 public:
  // Opens the volume whose root is `root` ("C:\"). Fails with invalid_argument when it is not
  // NTFS and access_denied when the process is not elevated (use scanner v1 then).
  [[nodiscard]] static Result<std::unique_ptr<Win32NtfsVolume>> open(
      const std::filesystem::path& root);

  [[nodiscard]] std::filesystem::path root() const override { return root_; }
  [[nodiscard]] Result<index::VolumeGeometry> geometry() const override;
  [[nodiscard]] Result<void> read(std::uint64_t offset, std::span<std::byte> into) const override;
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>> file_record(
      std::uint64_t record) const override;
  [[nodiscard]] Result<std::optional<index::JournalState>> journal() const override;
  [[nodiscard]] Result<std::vector<std::byte>> read_journal(std::uint64_t id,
                                                            std::int64_t from) const override;
  [[nodiscard]] Result<std::vector<std::byte>> enumerate(std::uint64_t from) const override;

 private:
  Win32NtfsVolume(std::filesystem::path root, wil::unique_hfile volume,
                  wil::unique_virtualalloc_ptr<std::byte> buffer, std::uint32_t bytes_per_record)
      : root_(std::move(root)),
        volume_(std::move(volume)),
        buffer_(std::move(buffer)),
        bytes_per_record_(bytes_per_record) {}

  std::filesystem::path root_;
  wil::unique_hfile volume_;
  wil::unique_virtualalloc_ptr<std::byte> buffer_;  // the aligned read buffer
  std::uint32_t bytes_per_record_;
};

}  // namespace myclean::platform
