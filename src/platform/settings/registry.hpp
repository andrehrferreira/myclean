#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {

// REG_SZ, REG_EXPAND_SZ (unexpanded) and REG_DWORD values under HKEY_CURRENT_USER ("HKCU\...")
// and HKEY_LOCAL_MACHINE ("HKLM\..."), in the 64-bit view. HKCU is this process's user, so it
// must run in the owner's context (the tray) for the owner's keys, unless `user_sid` names the
// user: HKCU is then HKEY_USERS\<user_sid> (the service reading the owner's loaded hive). HKLM
// writes need elevation.
class Win32Registry final : public placement::IRegistry {
 public:
  Win32Registry() = default;
  explicit Win32Registry(std::string user_sid) : user_sid_(std::move(user_sid)) {}

  [[nodiscard]] Result<std::optional<placement::RegistryValue>> read(
      std::string_view key, std::string_view name) const override;
  [[nodiscard]] Result<void> write(std::string_view key, std::string_view name,
                                   const placement::RegistryValue& value) override;
  [[nodiscard]] Result<void> remove(std::string_view key, std::string_view name) override;
  [[nodiscard]] Result<std::vector<std::string>> subkeys(std::string_view key) const override;

 private:
  std::string user_sid_;  // empty: HKCU is this process's user
};

}  // namespace myclean::platform
