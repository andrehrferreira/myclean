#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {

// The user environment variables in HKEY_CURRENT_USER\Environment, as REG_SZ or REG_EXPAND_SZ.
// It must run in the owner's context (the tray): in the service, HKEY_CURRENT_USER is the
// LocalSystem profile. expand() uses this process's environment; announce() broadcasts
// WM_SETTINGCHANGE "Environment" and waits at most 5 s for each window.
class Win32UserEnvironment final : public placement::IUserEnvironment {
 public:
  Win32UserEnvironment() = default;

  [[nodiscard]] Result<std::optional<placement::EnvValue>> get(
      std::string_view name) const override;
  [[nodiscard]] Result<void> set(std::string_view name, const placement::EnvValue& value) override;
  [[nodiscard]] Result<void> remove(std::string_view name) override;
  [[nodiscard]] Result<std::filesystem::path> expand(std::string_view text) const override;
  [[nodiscard]] Result<void> announce() override;
};

// Small text files read and written whole. A write goes to "<file>.myclean-new" first and then
// replaces the file, so a reader never sees half of it.
class Win32TextFiles final : public placement::ITextFiles {
 public:
  Win32TextFiles() = default;

  [[nodiscard]] Result<std::optional<std::string>> read(
      const std::filesystem::path& path) const override;
  [[nodiscard]] Result<void> write(const std::filesystem::path& path,
                                   std::string_view text) override;
  [[nodiscard]] Result<void> remove(const std::filesystem::path& path) override;
};

}  // namespace myclean::platform
