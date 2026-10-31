#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

#include "core/index/usn.hpp"
#include "core/index/volume.hpp"
#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/space/scanner.hpp"

namespace myclean::index {

// Reads the whole $MFT of `volume` in one sequential pass (4 MiB reads along its runs) and
// builds the index from it. Fails with invalid_state when `stop` is requested.
[[nodiscard]] Result<VolumeIndex> read_mft(const INtfsVolume& volume,
                                           const std::stop_token& stop = {});

// Calls `sink` for every file FSCTL_ENUM_USN_DATA lists (one name per file, no sizes). Not used
// to build the index; the X7 measurement compares it with read_mft.
[[nodiscard]] Result<void> enumerate_files(const INtfsVolume& volume,
                                           const std::function<void(const UsnRecord&)>& sink,
                                           const std::stop_token& stop = {});

enum class RefreshKind {
  updated,  // journal records were applied (possibly none)
  rebuilt,  // the index was read again from the $MFT
};

struct RefreshOutcome {
  RefreshKind kind = RefreshKind::updated;
  std::size_t records = 0;  // journal records applied
  std::string reason;       // why it was rebuilt
};

// A volume index kept current by the USN journal.
class IndexedVolume {
 public:
  // Notes the journal position, then reads the $MFT: changes made while it is read are applied
  // again by the next refresh, which is harmless because a refresh re-reads each file.
  [[nodiscard]] static Result<IndexedVolume> build(const INtfsVolume& volume,
                                                   const std::stop_token& stop = {});

  // Applies the journal records written since the build or the last refresh. Rebuilds from the
  // $MFT instead when the journal cannot cover the gap: the volume has no journal, it was
  // deleted or recreated (another id), it wrapped past the records not read yet, or those were
  // overwritten while reading; and when more than a quarter of the index is dead nodes.
  [[nodiscard]] Result<RefreshOutcome> refresh(const INtfsVolume& volume,
                                               const std::stop_token& stop = {});

  [[nodiscard]] const VolumeIndex& index() const { return index_; }
  // The journal and the USN the next refresh starts at; nullopt without a journal.
  [[nodiscard]] const std::optional<JournalState>& journal() const { return journal_; }

 private:
  IndexedVolume(VolumeIndex index, std::optional<JournalState> journal)
      : index_(std::move(index)), journal_(journal) {}

  VolumeIndex index_;
  std::optional<JournalState> journal_;
};

// The folder tree of `folder`: from `index` when it is given and holds the folder's volume,
// otherwise from scanner v1 (the fallback when the service is not elevated or the volume is
// not NTFS).
[[nodiscard]] Result<space::ScanResult> scan_tree(const VolumeIndex* index,
                                                  const space::IDirectoryReader& reader,
                                                  const std::filesystem::path& folder,
                                                  const space::ScanOptions& options = {},
                                                  const std::stop_token& stop = {});

}  // namespace myclean::index
