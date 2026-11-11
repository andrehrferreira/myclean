#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/security/extensions.hpp"
#include "core/security/network.hpp"
#include "core/security/persistence.hpp"
#include "core/security/verdicts.hpp"

// The intruder scan's Win32 readers (core/security). Everything here reads, except
// Win32Defender::scan_file, which starts a Defender scan that reports and removes nothing, and
// the VirusTotal and reverse-DNS lookups, which the service calls only when the owner turned
// them on. Nothing shows a window.
namespace myclean::platform {

// The owner's folders as the service sees them: from the ProfileList and the owner's User Shell
// Folders in HKEY_USERS\<sid>; with an empty SID, this process's user.
struct OwnerFolders {
  std::filesystem::path profile;        // C:\Users\<owner>
  std::filesystem::path local_appdata;  // ...\AppData\Local
  std::filesystem::path roaming;        // ...\AppData\Roaming
};

[[nodiscard]] Result<OwnerFolders> owner_folders(const std::string& user_sid);

// WMI event subscriptions (root\subscription: __FilterToConsumerBinding, __EventFilter,
// __EventConsumer), IFEO Debugger values, Winlogon Shell and Userinit (HKLM and the owner's
// hive) and AppInit_DLLs, in the 64-bit registry view plus WOW6432Node.
class Win32PersistenceSystem final : public security::IPersistenceSystem {
 public:
  explicit Win32PersistenceSystem(std::string user_sid = {}) : user_sid_(std::move(user_sid)) {}

  [[nodiscard]] security::PersistenceSnapshot read() const override;

 private:
  std::string user_sid_;
};

// Targets as Windows would find them for the owner: %USERPROFILE%, %APPDATA%, %LOCALAPPDATA%,
// %TEMP% and %TMP% from the owner's folders, other variables from the machine's environment; a
// bare file name in System32, then the Windows folder (also with ".exe" added).
class Win32PathResolver final : public security::IPathResolver {
 public:
  explicit Win32PathResolver(const std::string& user_sid = {});

  [[nodiscard]] std::optional<std::filesystem::path> resolve(
      std::string_view target) const override;

 private:
  std::optional<OwnerFolders> folders_;
};

// The owner's Chrome, Edge and Brave profiles (every folder of User Data with a Preferences
// file, except System Profile and Guest Profile), Firefox profiles (%APPDATA%\Mozilla\Firefox\
// Profiles\*\extensions.json), and the ExtensionInstallForcelist of each browser under
// HKLM\SOFTWARE\Policies and the owner's HKCU\SOFTWARE\Policies. Files are read shared, so an
// open browser is not disturbed.
class Win32BrowserProfiles final : public security::IBrowserProfiles {
 public:
  explicit Win32BrowserProfiles(std::string user_sid = {}) : user_sid_(std::move(user_sid)) {}

  [[nodiscard]] security::BrowserSnapshot read() const override;

 private:
  std::string user_sid_;
};

// Microsoft Defender: the detection history through WMI (root\Microsoft\Windows\Defender,
// MSFT_MpThreatDetection joined with MSFT_MpThreat) and the file scan of MpCmdRun.exe (the
// current platform's, from HKLM\SOFTWARE\Microsoft\Windows Defender InstallLocation, else
// %ProgramFiles%\Windows Defender) with -DisableRemediation, without a window, ended after
// `timeout`.
class Win32Defender final : public security::IDefender {
 public:
  explicit Win32Defender(std::chrono::milliseconds timeout = std::chrono::minutes{15})
      : timeout_(timeout) {}

  [[nodiscard]] Result<std::vector<security::Detection>> detections() const override;
  [[nodiscard]] Result<security::FileScan> scan_file(const std::filesystem::path& file,
                                                     const std::stop_token& stop) override;

 private:
  std::chrono::milliseconds timeout_;
};

// The VirusTotal v3 lookup over WinHTTP: GET https://www.virustotal.com/api/v3/files/<sha256>
// with the x-apikey header. Only the hash and the key leave the machine.
class Win32HashLookup final : public security::IHashLookup {
 public:
  [[nodiscard]] Result<std::optional<security::HashReport>> lookup(
      std::string_view sha256, std::string_view api_key) override;
};

// Reverse DNS through getnameinfo (NI_NAMEREQD).
class Win32Resolver final : public security::IResolver {
 public:
  Win32Resolver();
  Win32Resolver(const Win32Resolver&) = delete;
  Win32Resolver& operator=(const Win32Resolver&) = delete;
  Win32Resolver(Win32Resolver&&) = delete;
  Win32Resolver& operator=(Win32Resolver&&) = delete;
  ~Win32Resolver() override;

  [[nodiscard]] std::optional<std::string> name(std::string_view address) override;

 private:
  bool started_ = false;  // WSAStartup succeeded
};

// The text of %ProgramData%\MyClean\security.toml; nullopt when it does not exist. Fails with
// refused unless administrators alone can write to it (platform::check_admin_write_only), since
// it can turn on lookups that send data off the machine.
[[nodiscard]] Result<std::optional<std::string>> read_security_settings();

}  // namespace myclean::platform
