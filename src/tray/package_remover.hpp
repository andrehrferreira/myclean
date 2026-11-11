#pragma once

#include <stop_token>
#include <string>

#include "core/model/result.hpp"
#include "core/user_tasks/appx.hpp"

namespace myclean::tray {

// user_tasks::IPackageRemover over C++/WinRT's PackageManager for the tray's user (the owner):
// FindPackageForUser to check, RemovePackageAsync to remove, as Settings > Apps does. Only an
// approved uninstall plan reaches it; the tests never remove a package.
class Win32PackageRemover final : public user_tasks::IPackageRemover {
 public:
  [[nodiscard]] Result<bool> installed(const std::string& full_name) const override;
  [[nodiscard]] Result<void> remove(const std::string& full_name,
                                    const std::stop_token& stop) override;
};

}  // namespace myclean::tray
