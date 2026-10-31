#pragma once

#include <vector>

#include "core/model/result.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::platform {

// Mounted volumes from FindFirstVolumeW, GetVolumePathNamesForVolumeNameW and
// GetDiskFreeSpaceExW, mapped to physical disks with IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS (the
// volume is opened without access rights, so no elevation is needed). Critical-error dialogs
// ("insert a disk") are suppressed while it runs.
class Win32VolumeInfo final : public telemetry::IVolumeInfo {
 public:
  Win32VolumeInfo() = default;

  [[nodiscard]] Result<std::vector<telemetry::VolumeStatus>> volumes() const override;
};

}  // namespace myclean::platform
