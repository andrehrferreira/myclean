#pragma once

#include <vector>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {

// The system space settings, read and changed as the service (LocalSystem):
//   hibernation   HKLM\SYSTEM\CurrentControlSet\Control\Power: HibernateEnabled (else
//                 HibernateEnabledDefault), HiberFileType (1 reduced, 2 or absent full) and
//                 HiberFileSizePercent; the size of <system drive>\hiberfil.sys from its
//                 directory entry; the memory from GlobalMemoryStatusEx. powercfg's own output
//                 is localized, so it is not parsed.
//   page files    WMI root\cimv2: Win32_ComputerSystem.AutomaticManagedPagefile,
//                 Win32_PageFileSetting and Win32_PageFileUsage; set_page_files() enables
//                 SeCreatePagefilePrivilege and writes them in the order page_file_steps() shows.
//   shadow storage  Win32_ShadowStorage (needs administrator rights), volumes named by drive
//                 letter ("C:") when they have one.
class Win32SystemSpace final : public placement::ISystemSpace {
 public:
  Win32SystemSpace() = default;

  [[nodiscard]] Result<placement::HibernationState> hibernation() const override;
  [[nodiscard]] Result<placement::PageFileState> page_files() const override;
  [[nodiscard]] Result<void> set_page_files(const placement::PageFileConfig& config) override;
  [[nodiscard]] Result<std::vector<placement::ShadowStorage>> shadow_storage() const override;
};

}  // namespace myclean::platform
