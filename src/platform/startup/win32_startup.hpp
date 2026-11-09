#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/model/result.hpp"
#include "core/startup/startup.hpp"
#include "platform/trust/win_image_inspector.hpp"

namespace myclean::platform {

// The automatic start sources of this machine through Win32 (spike X6, docs/spikes/X6-startup.md):
// the Run and RunOnce keys of HKCU and HKLM in the 64-bit view plus their WOW6432Node copies (never
// a 32-bit view as well, which would count them twice), the user's and the common Startup folder,
// the StartupApproved values, every scheduled task (ITaskService) and every Win32 service.
// Signers come from WinImageInspector, which never touches the network.
//
// HKCU and the user's Startup folder are this process's user, unless `user_sid` names the user:
// HKCU is then HKEY_USERS\<user_sid> (the service reading the owner's loaded hive) and the
// Startup folder is that profile's, from its User Shell Folders. Writes to HKLM, tasks and
// services need elevation.
class Win32StartupSystem final : public startup::IStartupSystem {
 public:
  explicit Win32StartupSystem(std::string user_sid = {}) : user_sid_(std::move(user_sid)) {}

  [[nodiscard]] Result<startup::Snapshot> read() const override;
  [[nodiscard]] Result<void> set_approved(startup::Scope scope, startup::ApprovedKey key,
                                          std::string_view name,
                                          const startup::ApprovedValue& value) override;
  [[nodiscard]] Result<void> set_task_enabled(std::string_view path, bool enabled) override;
  [[nodiscard]] Result<void> set_service_start(std::string_view name,
                                               const startup::ServiceStart& start) override;

 private:
  std::string user_sid_;  // empty: HKCU is this process's user
  WinImageInspector inspector_;
};

// The pure pieces of the reader and writer, exposed for tests.
namespace startup_detail {

// The executable of a service command line (lpBinaryPathName): the quoted part, else the text up
// to the first ".exe" that ends a word (unquoted paths with spaces exist), else up to the first
// space. "\??\" is dropped and a leading "\SystemRoot\" becomes "%SystemRoot%\"; environment
// variables stay unexpanded.
[[nodiscard]] std::string executable_of(std::string_view command);

// Whether `executable` is a shared service host (its file name is svchost.exe, any case).
[[nodiscard]] bool is_svchost(std::string_view executable);

// `text` without one pair of surrounding double quotes.
[[nodiscard]] std::string_view unquote(std::string_view text);

// "<path> <arguments>", or just the path when there are no arguments.
[[nodiscard]] std::string join_command(std::string_view path, std::string_view arguments);

// `text` with every %USERPROFILE% (any case) replaced by `profile`.
[[nodiscard]] std::string expand_profile(std::string_view text, std::string_view profile);

// The StartType of a SERVICE_*_START code; nullopt for an unknown code.
[[nodiscard]] std::optional<startup::StartType> start_type_of(std::uint32_t code);
// The SERVICE_*_START code of `type`.
[[nodiscard]] std::uint32_t start_type_code(startup::StartType type);
// The delayed-start flag to write: only an automatic service can be delayed.
[[nodiscard]] bool delayed_flag(const startup::ServiceStart& start);

}  // namespace startup_detail

}  // namespace myclean::platform
