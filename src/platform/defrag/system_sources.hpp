#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/drives/actions.hpp"
#include "core/drives/analysis.hpp"
#include "core/drives/history.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// drives::IOptimizerLog over the Application event log (EvtQuery, newest first, at most
// max_events): events 258 and 264 of Microsoft-Windows-Defrag. Readable without elevation.
class Win32OptimizerLog final : public drives::IOptimizerLog {
 public:
  static constexpr std::uint32_t max_events = 10'000;

  Win32OptimizerLog() = default;

  [[nodiscard]] Result<std::vector<drives::OptimizerEvent>> events() const override;
};

// drives::ITrimSupport: IOCTL_STORAGE_QUERY_PROPERTY (StorageDeviceTrimProperty) on
// \\.\PhysicalDriveN opened without access rights, and the NTFS DisableDeleteNotification value
// under HKLM\SYSTEM\CurrentControlSet\Control\FileSystem. Both readable without elevation.
class Win32TrimSupport final : public drives::ITrimSupport {
 public:
  Win32TrimSupport() = default;

  [[nodiscard]] std::optional<bool> trim_supported(std::uint32_t disk_number) const override;
  [[nodiscard]] std::optional<bool> ntfs_trim_enabled() const override;
};

// drives::IPowerSource over GetSystemPowerStatus: on battery when the AC line is offline; a
// computer without a system battery never is. Fails when Windows does not know.
class Win32PowerSource final : public drives::IPowerSource {
 public:
  Win32PowerSource() = default;

  [[nodiscard]] Result<bool> on_battery() const override;
};

}  // namespace myclean::platform
