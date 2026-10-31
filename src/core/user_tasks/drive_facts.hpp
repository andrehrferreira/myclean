#pragma once

#include <optional>
#include <vector>

#include "core/placement/drives.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::user_tasks {

// The volumes with a drive letter as relocation targets, from what the tray polls: the volumes
// of metrics.latest and the drives of disks.health. A volume is healthy only when the health of
// every disk under it was read and is `good`; without disk health every volume is of unknown
// health and refused as a target. The drive class is not known in the tray and stays unknown:
// apply-time checks need only health and room (placement::unsuitable).
[[nodiscard]] std::vector<placement::TargetVolume> target_volumes(
    const std::vector<tray_model::VolumeRow>& volumes,
    const std::optional<std::vector<tray_model::DriveHealthRow>>& drives);

}  // namespace myclean::user_tasks
