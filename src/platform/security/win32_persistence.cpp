#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// clang-format off
#include <windows.h>  // first: the other Win32 headers need its types
#include <KnownFolders.h>
#include <ShlObj.h>
#include <wil/resource.h>
// clang-format on

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"
#include "core/security/persistence.hpp"
#include "core/startup/startup.hpp"
#include "platform/security/win32_security.hpp"
#include "platform/security/wmi.hpp"
#include "platform/settings/registry.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {
namespace {

using settings_detail::narrow;
using settings_detail::widen;

constexpr std::string_view nt_current = R"(\SOFTWARE\Microsoft\Windows NT\CurrentVersion\)";
constexpr std::string_view nt_current32 =
    R"(\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\)";
constexpr std::size_t max_script_text = 1024;

std::wstring expand(const std::wstring& text) {
  const DWORD length = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
  if (length == 0) {
    return text;
  }
  std::wstring expanded(length, L'\0');
  const DWORD written = ::ExpandEnvironmentStringsW(text.c_str(), expanded.data(), length);
  if (written == 0 || written > length) {
    return text;
  }
  expanded.resize(written - 1);
  return expanded;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  wil::unique_cotaskmem_string path;
  if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, path.put()))) {
    return {};
  }
  return std::filesystem::path{path.get()};
}

std::wstring system_folder(bool windows) {
  std::array<wchar_t, MAX_PATH + 1> buffer{};
  const UINT length = windows ? ::GetWindowsDirectoryW(buffer.data(), MAX_PATH + 1)
                              : ::GetSystemDirectoryW(buffer.data(), MAX_PATH + 1);
  if (length == 0 || length > MAX_PATH) {
    return {};
  }
  return {buffer.data(), length};
}

// `text` with every `variable` (any case) replaced by `value`.
std::wstring replace_variable(std::wstring text, std::wstring_view variable,
                              const std::wstring& value) {
  if (value.empty()) {
    return text;
  }
  for (std::size_t at = 0; at + variable.size() <= text.size();) {
    const std::wstring_view part = std::wstring_view(text).substr(at, variable.size());
    if (::CompareStringOrdinal(part.data(), static_cast<int>(part.size()), variable.data(),
                               static_cast<int>(variable.size()), TRUE) == CSTR_EQUAL) {
      text.replace(at, variable.size(), value);
      at += value.size();
    } else {
      ++at;
    }
  }
  return text;
}

std::string key_text(std::string_view hive, std::string_view path, std::string_view rest) {
  return std::string(hive) + std::string(path) + std::string(rest);
}

// The value or nothing: a key or value that does not exist, or cannot be read, adds nothing.
std::optional<placement::RegistryValue> read_value(const Win32Registry& registry,
                                                   const std::string& key, std::string_view name) {
  Result<std::optional<placement::RegistryValue>> value = registry.read(key, name);
  return value ? std::move(*value) : std::nullopt;
}

std::optional<std::string> value_of(const Win32Registry& registry, const std::string& key,
                                    std::string_view name) {
  const std::optional<placement::RegistryValue> found = read_value(registry, key, name);
  if (!found) {
    return std::nullopt;
  }
  return found->text;
}

// What a consumer runs: CommandLineTemplate (else ExecutablePath) of a command-line consumer,
// ScriptFileName (else the script text, cut) of a script consumer.
std::string consumer_command(IWbemClassObject& row, std::string_view kind) {
  std::string command;
  if (kind == "CommandLineEventConsumer") {
    command = security_detail::wmi_text(row, L"CommandLineTemplate");
    if (command.empty()) {
      command = security_detail::wmi_text(row, L"ExecutablePath");
    }
  } else if (kind == "ActiveScriptEventConsumer") {
    command = security_detail::wmi_text(row, L"ScriptFileName");
    if (command.empty()) {
      command = security_detail::wmi_text(row, L"ScriptText").substr(0, max_script_text);
    }
  }
  return command;
}

void read_wmi(security::PersistenceSnapshot& snapshot) {
  const security_detail::ComScope com;
  if (Result<void> status = com.status(); !status) {
    snapshot.problems.push_back("WMI subscriptions: " + status.error().message);
    return;
  }
  constexpr std::wstring_view space = LR"(ROOT\subscription)";
  auto filters = security_detail::wmi_query(space, L"SELECT * FROM __EventFilter");
  auto consumers = security_detail::wmi_query(space, L"SELECT * FROM __EventConsumer");
  auto bindings = security_detail::wmi_query(space, L"SELECT * FROM __FilterToConsumerBinding");
  for (const Result<std::vector<security_detail::WmiRow>>* result :
       {&filters, &consumers, &bindings}) {
    if (!*result) {
      snapshot.problems.push_back("WMI subscriptions: " + result->error().message);
      return;
    }
  }
  // By relative path ("__EventFilter.Name=\"x\""), which the bindings' references end with.
  struct Filter {
    std::string name;
    std::string query;
  };
  std::map<std::string, Filter> filter_by_path;
  for (const security_detail::WmiRow& row : *filters) {
    filter_by_path.emplace(security_detail::wmi_text(*row, L"__RELPATH"),
                           Filter{.name = security_detail::wmi_text(*row, L"Name"),
                                  .query = security_detail::wmi_text(*row, L"Query")});
  }
  const auto relative = [](const std::string& reference) {
    const std::size_t colon = reference.find(':');
    return colon == std::string::npos ? reference : reference.substr(colon + 1);
  };
  std::multimap<std::string, std::string> filters_of_consumer;  // consumer path -> filter path
  for (const security_detail::WmiRow& row : *bindings) {
    filters_of_consumer.emplace(relative(security_detail::wmi_text(*row, L"Consumer")),
                                relative(security_detail::wmi_text(*row, L"Filter")));
  }
  for (const security_detail::WmiRow& row : *consumers) {
    const std::string path = security_detail::wmi_text(*row, L"__RELPATH");
    const std::string kind = security_detail::wmi_text(*row, L"__CLASS");
    const security::WmiSubscription subscription{
        .consumer_class = kind,
        .consumer = security_detail::wmi_text(*row, L"Name"),
        .command = consumer_command(*row, kind)};
    const auto [first, last] = filters_of_consumer.equal_range(path);
    if (first == last) {
      snapshot.wmi.push_back(subscription);
      continue;
    }
    for (auto binding = first; binding != last; ++binding) {
      security::WmiSubscription bound = subscription;
      if (const auto filter = filter_by_path.find(binding->second);
          filter != filter_by_path.end()) {
        bound.filter = filter->second.name;
        bound.query = filter->second.query;
      } else {
        bound.filter = binding->second;
      }
      snapshot.wmi.push_back(std::move(bound));
    }
  }
}

void read_ifeo(security::PersistenceSnapshot& snapshot) {
  const Win32Registry machine;
  for (const bool wow64 : {false, true}) {
    const std::string root =
        key_text("HKLM", wow64 ? nt_current32 : nt_current, "Image File Execution Options");
    const Result<std::vector<std::string>> images = machine.subkeys(root);
    if (!images) {
      if (images.error().code != Errc::not_found) {
        snapshot.problems.push_back("Image File Execution Options: " + images.error().message);
      }
      continue;
    }
    for (const std::string& image : *images) {
      std::string key = root;
      key.append("\\").append(image);
      if (std::optional<std::string> debugger = value_of(machine, key, "Debugger");
          debugger && !debugger->empty()) {
        snapshot.ifeo.push_back(security::IfeoDebugger{
            .wow64 = wow64, .image = image, .debugger = std::move(*debugger)});
      }
    }
  }
}

void read_winlogon(security::PersistenceSnapshot& snapshot, const std::string& user_sid) {
  const Win32Registry machine;
  const std::string key = key_text("HKLM", nt_current, "Winlogon");
  for (const std::string_view name : {std::string_view("Shell"), std::string_view("Userinit")}) {
    if (std::optional<std::string> value = value_of(machine, key, name)) {
      snapshot.winlogon.push_back(security::WinlogonValue{
          .scope = startup::Scope::machine, .name = std::string(name), .value = std::move(*value)});
    }
  }
  const Win32Registry user{user_sid};  // an empty SID is this process's user
  if (std::optional<std::string> shell =
          value_of(user, key_text("HKCU", nt_current, "Winlogon"), "Shell")) {
    snapshot.winlogon.push_back(security::WinlogonValue{
        .scope = startup::Scope::user, .name = "Shell", .value = std::move(*shell)});
  }
}

void read_appinit(security::PersistenceSnapshot& snapshot) {
  const Win32Registry machine;
  for (const bool wow64 : {false, true}) {
    const std::string key = key_text("HKLM", wow64 ? nt_current32 : nt_current, "Windows");
    const std::optional<std::string> dlls = value_of(machine, key, "AppInit_DLLs");
    if (!dlls) {
      continue;
    }
    const std::optional<std::string> load = value_of(machine, key, "LoadAppInit_DLLs");
    snapshot.appinit.push_back(
        security::AppInit{.wow64 = wow64, .load_enabled = load == "1", .dlls = *dlls});
  }
}

}  // namespace

Result<OwnerFolders> owner_folders(const std::string& user_sid) {
  if (user_sid.empty()) {
    return OwnerFolders{.profile = known_folder(FOLDERID_Profile),
                        .local_appdata = known_folder(FOLDERID_LocalAppData),
                        .roaming = known_folder(FOLDERID_RoamingAppData)};
  }
  const Win32Registry machine;
  const std::optional<std::string> profile = value_of(
      machine, R"(HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\)" + user_sid,
      "ProfileImagePath");
  if (!profile) {
    return fail(Errc::not_found, "no profile is registered for " + user_sid);
  }
  const std::wstring profile_path = expand(widen(*profile));
  const Win32Registry user{user_sid};
  const auto shell_folder = [&](std::string_view name, std::wstring_view fallback) {
    const std::optional<std::string> folder = value_of(
        user, R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)",
        name);
    std::wstring text = folder ? widen(*folder) : std::wstring(fallback);
    return std::filesystem::path{
        expand(replace_variable(std::move(text), L"%USERPROFILE%", profile_path))};
  };
  return OwnerFolders{
      .profile = profile_path,
      .local_appdata = shell_folder("Local AppData", LR"(%USERPROFILE%\AppData\Local)"),
      .roaming = shell_folder("AppData", LR"(%USERPROFILE%\AppData\Roaming)")};
}

security::PersistenceSnapshot Win32PersistenceSystem::read() const {
  security::PersistenceSnapshot snapshot{.system_folder = narrow(system_folder(false))};
  read_wmi(snapshot);
  read_ifeo(snapshot);
  read_winlogon(snapshot, user_sid_);
  read_appinit(snapshot);
  return snapshot;
}

Win32PathResolver::Win32PathResolver(const std::string& user_sid) {
  if (Result<OwnerFolders> folders = owner_folders(user_sid); folders) {
    folders_ = std::move(*folders);
  }
}

std::optional<std::filesystem::path> Win32PathResolver::resolve(std::string_view target) const {
  if (target.empty()) {
    return std::nullopt;
  }
  std::wstring text = widen(target);
  if (folders_) {
    const std::wstring temp = (folders_->local_appdata / L"Temp").native();
    text = replace_variable(std::move(text), L"%USERPROFILE%", folders_->profile.native());
    text = replace_variable(std::move(text), L"%LOCALAPPDATA%", folders_->local_appdata.native());
    text = replace_variable(std::move(text), L"%APPDATA%", folders_->roaming.native());
    text = replace_variable(std::move(text), L"%TEMP%", temp);
    text = replace_variable(std::move(text), L"%TMP%", temp);
  }
  const std::filesystem::path path{expand(text)};
  std::vector<std::filesystem::path> candidates;
  if (path.has_parent_path()) {
    candidates.push_back(path);
  } else {
    for (const bool windows : {false, true}) {
      const std::filesystem::path folder{system_folder(windows)};
      candidates.push_back(folder / path);
      if (!path.has_extension()) {
        candidates.push_back(folder / (path.native() + L".exe"));
      }
    }
  }
  for (const std::filesystem::path& candidate : candidates) {
    std::error_code error;
    if (candidate.is_absolute() && std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
  }
  return std::nullopt;
}

}  // namespace myclean::platform
