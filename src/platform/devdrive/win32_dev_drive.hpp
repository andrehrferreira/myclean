#pragma once

#include <vector>

#include "core/devdrive/advisor.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// The Windows build from HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion (CurrentBuildNumber,
// UBR) and, for every fixed volume with a drive letter, its file system (GetVolumeInformationW)
// and, on ReFS, whether it is a developer volume and trusted (FSCTL_QUERY_PERSISTENT_VOLUME_STATE
// on its root folder, opened for attributes only). Changes nothing; reading the volume state may
// need administrator rights, which the service has: without them the volume carries a note.
class Win32DevDriveProbe final : public devdrive::IDevDriveProbe {
 public:
  Win32DevDriveProbe() = default;

  [[nodiscard]] Result<devdrive::Support> support() const override;
  [[nodiscard]] Result<std::vector<devdrive::VolumeState>> volumes() const override;
};

}  // namespace myclean::platform
