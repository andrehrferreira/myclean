#pragma once

#include <cstdint>
#include <vector>

#include "core/health/reading.hpp"
#include "core/health/source.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// Health of every \\.\PhysicalDriveN (0-63), read-only (docs/spikes/X1-disk-health.md):
//   identity, bus, temperature  IOCTL_STORAGE_QUERY_PROPERTY (device descriptor, temperature)
//   NVMe                        IOCTL_STORAGE_QUERY_PROPERTY, protocol-specific log page 02h
//   SATA                        SMART_RCV_DRIVE_DATA_EX (SMART READ DATA) and
//                               IOCTL_STORAGE_PREDICT_FAILURE
//   USB                         the NVMe and SMART queries above, then SAT: IOCTL_SCSI_PASS_THROUGH
//                               with ATA PASS-THROUGH(16) carrying SMART READ DATA
//   no health data              Windows reliability counters (WMI MSFT_StorageReliabilityCounter)
// Every query except SAT and WMI works on a handle opened without access rights, so without
// elevation. SAT needs a read/write handle and WMI an administrator; without them the probe
// says so in DriveProbe::problem. Only read commands are ever sent.
class Win32DiskHealthSource final : public health::IDiskHealthSource {
 public:
  Win32DiskHealthSource() = default;

  [[nodiscard]] Result<std::vector<health::DriveProbe>> probe() const override;
};

// The reliability counters Windows keeps for physical disk `disk_number`. Fails with
// access_denied without administrator rights and not_found when Windows has none.
[[nodiscard]] Result<health::ReliabilityCounters> reliability_counters(std::uint32_t disk_number);

}  // namespace myclean::platform
