#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/backup/backup.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::platform {

// backup::IBackupSystem on Windows, the values of spike X8 (docs/spikes/X8-backups.md), for the
// owner `owner_sid` (S-1-5-...): the owner's hive is read under HKEY_USERS\<owner_sid> and the
// owner's profile folder comes from ProfileList, so the service (LocalSystem) reads the owner's
// File History and OneDrive state. Everything here only reads:
//   file_history    <Local AppData>\Microsoft\Windows\FileHistory\Configuration\Config1.xml
//                   (Local AppData from the owner's User Shell Folders): its <TargetUrl> and the
//                   newest last write of the Configuration folder's files
//   image_versions  `wbadmin get versions` without a window, judged by its exit code only: 0
//                   versions exist; -2 (access denied, as non-elevated) fails with
//                   access_denied; any other code: none
//   restore_points  WMI root\default SystemRestore (CreationTime, Description)
//   shadow_copies   WMI root\cimv2 Win32_ShadowCopy (VolumeName, InstallDate)
//   onedrive        HKCU\Software\Microsoft\OneDrive\Accounts\<account>: UserFolder,
//                   SignInErrorStateStartTimestamp (Unix seconds); a known folder (Desktop,
//                   Personal, My Pictures of User Shell Folders, expanded) under UserFolder is
//                   protected
//   installed_programs  DisplayName of the Uninstall keys: HKLM 64-bit, HKLM WOW6432Node and the
//                   owner's HKCU
//   volumes         the fixed drives with a letter, with their labels
class Win32BackupSystem final : public backup::IBackupSystem {
 public:
  explicit Win32BackupSystem(std::string owner_sid) : owner_sid_(std::move(owner_sid)) {}

  [[nodiscard]] Result<backup::FileHistoryFacts> file_history() const override;
  [[nodiscard]] Result<bool> image_versions() const override;
  [[nodiscard]] Result<std::vector<backup::RestorePoint>> restore_points() const override;
  [[nodiscard]] Result<std::vector<backup::ShadowCopy>> shadow_copies() const override;
  [[nodiscard]] Result<std::vector<backup::OneDriveAccount>> onedrive() const override;
  [[nodiscard]] Result<std::vector<std::string>> installed_programs() const override;
  [[nodiscard]] Result<std::vector<backup::Volume>> volumes() const override;

 private:
  std::string owner_sid_;
};

// A WMI datetime ("20261012143022.000000-180": local time and its offset from UTC in minutes) as
// a WallTime; nullopt when it is not one.
[[nodiscard]] std::optional<WallTime> cim_time(std::wstring_view text);

}  // namespace myclean::platform
