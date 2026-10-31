#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::telemetry {

// A mounted volume. Strings are UTF-8.
struct VolumeStatus {
  std::string id;                         // volume GUID path, "\\?\Volume{...}\"
  std::vector<std::string> mount_points;  // "C:\", "D:\mnt\data\"; never empty
  Bytes size;
  Bytes free;
  std::vector<std::uint32_t> disks;  // numbers of the physical disks that hold its extents
};

// Implemented by src/platform/disk.
class IVolumeInfo {
 public:
  virtual ~IVolumeInfo() = default;
  IVolumeInfo(const IVolumeInfo&) = delete;
  IVolumeInfo& operator=(const IVolumeInfo&) = delete;
  IVolumeInfo(IVolumeInfo&&) = delete;
  IVolumeInfo& operator=(IVolumeInfo&&) = delete;

  // Every mounted volume whose size can be read. A volume without media (an empty card reader)
  // is left out.
  [[nodiscard]] virtual Result<std::vector<VolumeStatus>> volumes() const = 0;

 protected:
  IVolumeInfo() = default;
};

}  // namespace myclean::telemetry
