#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "core/map/disk_map.hpp"
#include "core/model/units.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"

// The placement advisor (scan-all spec, "Placement Advice"): what lives on the wrong kind of
// drive and where it should go. Read-only.
namespace myclean::scanall {

struct PlacementOptions {
  Bytes min_size{std::uint64_t{1} << 30U};      // folders moved off a failing drive
  Bytes min_project{std::uint64_t{64} << 20U};  // projects, games and caches
  std::chrono::days active{30};                 // a project changed within this is active
};

// Relocate findings for the drive `drive` (one of `drives`) whose tree is `tree` and whose map
// is `map` (null when it could not be built):
//   - health warning or critical: every folder of the root of at least min_size that is not a
//     system_location(), urgent, to the best healthy drive with room (any class but USB);
//   - an active project (a folder holding ".git", changed within `active`) on a QLC NVMe or USB
//     drive, outermost only, to a healthy NVMe or SATA SSD;
//   - a game library or game (map category games) on a USB drive, to a healthy NVMe or SATA SSD;
//   - on the system drive, a location the attribution relocates (TEMP, package caches, WSL and
//     Docker disks), through its relocation (relocate_cache, ref = the attribution's ref).
// A finding without a qualifying target is a manual-review finding saying so. On a failing
// drive only the first kind is given.
[[nodiscard]] std::vector<Finding> advise_placement(const space::ScanResult& tree,
                                                    const FolderTimes& times,
                                                    const map::DiskMap* map, const DriveInfo& drive,
                                                    std::span<const DriveInfo> drives, WallTime now,
                                                    const PlacementOptions& options = {});

}  // namespace myclean::scanall
