#pragma once

#include <cstdint>
#include <optional>

#include "core/placement/drives.hpp"

namespace myclean::platform {

// IOCTL_STORAGE_QUERY_PROPERTY StorageDeviceSeekPenaltyProperty on \\.\PhysicalDriveN, opened
// without access rights, so no elevation is needed.
class Win32SeekPenalty final : public placement::ISeekPenalty {
 public:
  Win32SeekPenalty() = default;

  [[nodiscard]] std::optional<bool> seek_penalty(std::uint32_t disk_number) const override;
};

}  // namespace myclean::platform
