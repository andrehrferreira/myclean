#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "core/model/units.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"

// Cold data (scan-all spec, "Cold Data"): large folders on an SSD that nothing has written for
// months, proposed for archiving to a healthy HDD. Read-only.
namespace myclean::scanall {

// When each folder of a tree last changed.
struct FolderTimes {
  // By folder index: the newest creation or last write of any file at or below the folder
  // (creation counts because a copied file keeps its old write time). Last access is not used.
  std::vector<WallTime> newest;
  // By folder index: every folder at or below it was listed, so `newest` is proven.
  std::vector<bool> complete;
};

[[nodiscard]] FolderTimes folder_times(const space::ScanResult& tree);

struct ColdOptions {
  Bytes min_size{std::uint64_t{1} << 30U};
  int min_age_months = 6;  // the newest change is older than this
};

// The largest cold folders of `tree` (the drive `drive`, one of `drives`): folders of at least
// min_size whose newest change is older than min_age_months, on an NVMe or SATA SSD. A folder
// is proposed only when its whole subtree was listed, and never below a system_location() or
// when a running process uses a folder inside it (`in_use`: their current and image folders);
// only the outermost cold folder is proposed, unless it holds backup-like folders (named like
// *backup*, or at least half .zip, .rar or .7z files directly inside) next to min_size or more of
// other content: then its subfolders are judged. Each is an archive finding with a move_folder plan
// to the healthy HDD with the most room that keeps 10 % free afterwards, or a manual-review
// finding when no HDD qualifies.
[[nodiscard]] std::vector<Finding> find_cold(const space::ScanResult& tree,
                                             const FolderTimes& times, const DriveInfo& drive,
                                             std::span<const DriveInfo> drives,
                                             std::span<const std::filesystem::path> in_use,
                                             WallTime now, const ColdOptions& options = {});

}  // namespace myclean::scanall
