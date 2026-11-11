#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/apps/apps.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::platform {

// apps::IAppSystem on Windows for the owner `owner_sid` (S-1-5-...); the service (LocalSystem)
// reads the owner's hive under HKEY_USERS. Everything here only reads:
//   appx          PackageManager.FindPackagesForUser(owner) (the caller's own packages when it is
//                 the owner): main packages only (no framework or resource package), with
//                 SignatureKind System marked, and the size of each package folder (its files,
//                 no reparse point followed), measured once per package
//   user_assist   HKCU\...\Explorer\UserAssist\{GUID}\Count: ROT13 value names, a known-folder
//                 GUID at the start replaced by its folder, the last run of each 72-byte value
//   prefetch      %SystemRoot%\Prefetch\*.pf, decompressed (XPRESS_HUFF, "MAM\x04") with
//                 RtlDecompressBufferEx, the executable's path from its volumes and file names
//                 (needs administrator rights)
//   bam           HKLM\SYSTEM\CurrentControlSet\Services\bam\State\UserSettings\<owner>: each
//                 value's device path (\Device\HarddiskVolumeN\...) as a drive path, the FILETIME
//                 of its first 8 bytes (needs administrator rights)
//   manufacturer  the SMBIOS system manufacturer that Win32_ComputerSystem.Manufacturer reports
//                 (HKLM\HARDWARE\DESCRIPTION\System\BIOS SystemManufacturer)
class Win32AppSystem final : public apps::IAppSystem {
 public:
  explicit Win32AppSystem(std::string owner_sid) : owner_sid_(std::move(owner_sid)) {}

  [[nodiscard]] Result<std::vector<apps::AppxPackage>> appx() const override;
  [[nodiscard]] Result<std::vector<apps::RunRecord>> user_assist() const override;
  [[nodiscard]] Result<std::vector<apps::RunRecord>> prefetch() const override;
  [[nodiscard]] Result<std::vector<apps::RunRecord>> bam() const override;
  [[nodiscard]] Result<std::string> manufacturer() const override;

 private:
  std::string owner_sid_;
  mutable std::mutex mutex_;                    // guards sizes_
  mutable std::map<std::string, Bytes> sizes_;  // package full name -> folder size
};

}  // namespace myclean::platform
