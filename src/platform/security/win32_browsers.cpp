#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/text.hpp"
#include "core/security/extensions.hpp"
#include "platform/security/win32_security.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {
namespace {

using security::Browser;
using settings_detail::narrow;
using settings_detail::widen;

constexpr std::size_t max_profile_file = std::size_t{64} * 1024 * 1024;

struct ChromiumBrowser {
  Browser browser;
  std::wstring_view user_data;  // below %LOCALAPPDATA%
  std::wstring_view policy;     // below SOFTWARE\Policies
};

// The Chromium browsers of the browsers knowledge pack (data/packs/browsers).
constexpr std::array<ChromiumBrowser, 3> chromium_browsers{{
    {.browser = Browser::chrome,
     .user_data = LR"(Google\Chrome\User Data)",
     .policy = LR"(Google\Chrome)"},
    {.browser = Browser::edge,
     .user_data = LR"(Microsoft\Edge\User Data)",
     .policy = LR"(Microsoft\Edge)"},
    {.browser = Browser::brave,
     .user_data = LR"(BraveSoftware\Brave-Browser\User Data)",
     .policy = LR"(BraveSoftware\Brave)"},
}};

// The file's text; nullopt when it does not exist. Read shared: the browser may have it open.
Result<std::optional<std::string>> read_text(const std::filesystem::path& file) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error)) {
    return std::optional<std::string>{};
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return fail(Errc::io_error, utf8_text(file) + ": cannot be opened");
  }
  std::string text;
  std::array<char, 65536> buffer{};
  while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
    text.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    if (text.size() > max_profile_file) {
      return fail(Errc::invalid_data, utf8_text(file) + ": larger than 64 MiB");
    }
  }
  return std::optional<std::string>{std::move(text)};
}
// The string values of `path` under `root`; none when the key does not exist.
std::vector<std::string> string_values(HKEY root, const std::wstring& path) {
  std::vector<std::string> values;
  wil::unique_hkey key;
  if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, key.put()) !=
      ERROR_SUCCESS) {
    return values;
  }
  DWORD count = 0;
  DWORD max_name = 0;
  DWORD max_data = 0;
  if (::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count,
                         &max_name, &max_data, nullptr, nullptr) != ERROR_SUCCESS) {
    return values;
  }
  std::vector<wchar_t> name(static_cast<std::size_t>(max_name) + 1);
  std::vector<std::byte> data(static_cast<std::size_t>(max_data) + sizeof(wchar_t));
  for (DWORD index = 0; index < count; ++index) {
    auto name_length = static_cast<DWORD>(name.size());
    auto data_size = static_cast<DWORD>(data.size());
    DWORD type = REG_NONE;
    if (::RegEnumValueW(key.get(), index, name.data(), &name_length, nullptr, &type,
                        reinterpret_cast<LPBYTE>(data.data()),  // NOLINT(*-reinterpret-cast): Win32
                        &data_size) != ERROR_SUCCESS ||
        type != REG_SZ) {
      continue;
    }
    std::wstring text(data_size / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
      text.pop_back();
    }
    values.push_back(narrow(text));
  }
  return values;
}

std::set<std::string> forced_ids(std::wstring_view policy, const std::string& user_sid) {
  std::set<std::string> ids;
  const std::wstring path =
      LR"(SOFTWARE\Policies\)" + std::wstring(policy) + L"\\ExtensionInstallForcelist";
  std::vector<std::string> values = string_values(HKEY_LOCAL_MACHINE, path);
  const std::vector<std::string> user =
      user_sid.empty() ? string_values(HKEY_CURRENT_USER, path)
                       : string_values(HKEY_USERS, widen(user_sid) + L"\\" + path);
  values.insert(values.end(), user.begin(), user.end());
  for (const std::string& value : values) {
    if (std::string id = security::forced_id(value); !id.empty()) {
      ids.insert(std::move(id));
    }
  }
  return ids;
}

bool skipped_profile(const std::wstring& name) {
  return name == L"System Profile" || name == L"Guest Profile";
}

void read_chromium(const ChromiumBrowser& browser, const std::filesystem::path& user_data,
                   security::BrowserSnapshot& snapshot) {
  std::error_code error;
  if (!std::filesystem::is_directory(user_data, error)) {
    return;
  }
  std::filesystem::directory_iterator entry{user_data, error};
  for (; !error && entry != std::filesystem::directory_iterator{}; entry.increment(error)) {
    std::error_code ignored;
    const std::filesystem::path folder = entry->path();
    if (!entry->is_directory(ignored) || skipped_profile(folder.filename().native()) ||
        !std::filesystem::is_regular_file(folder / L"Preferences", ignored)) {
      continue;
    }
    security::ChromiumProfile profile{.browser = browser.browser,
                                      .profile = utf8_text(folder.filename())};
    Result<std::optional<std::string>> preferences = read_text(folder / L"Preferences");
    Result<std::optional<std::string>> secure = read_text(folder / L"Secure Preferences");
    for (const Result<std::optional<std::string>>* result : {&preferences, &secure}) {
      if (!*result) {
        snapshot.problems.push_back(result->error().message);
      }
    }
    profile.preferences = preferences ? std::move(*preferences) : std::nullopt;
    profile.secure_preferences = secure ? std::move(*secure) : std::nullopt;
    snapshot.chromium.push_back(std::move(profile));
  }
  if (error) {
    snapshot.problems.push_back("listing " + utf8_text(user_data) + ": " + error.message());
  }
}

void read_firefox(const std::filesystem::path& profiles, security::BrowserSnapshot& snapshot) {
  std::error_code error;
  if (!std::filesystem::is_directory(profiles, error)) {
    return;
  }
  std::filesystem::directory_iterator entry{profiles, error};
  for (; !error && entry != std::filesystem::directory_iterator{}; entry.increment(error)) {
    std::error_code ignored;
    if (!entry->is_directory(ignored)) {
      continue;
    }
    Result<std::optional<std::string>> text = read_text(entry->path() / L"extensions.json");
    if (!text) {
      snapshot.problems.push_back(text.error().message);
      continue;
    }
    if (*text) {
      snapshot.firefox.push_back(security::FirefoxProfile{
          .profile = utf8_text(entry->path().filename()), .extensions_json = std::move(**text)});
    }
  }
  if (error) {
    snapshot.problems.push_back("listing " + utf8_text(profiles) + ": " + error.message());
  }
}

}  // namespace

security::BrowserSnapshot Win32BrowserProfiles::read() const {
  security::BrowserSnapshot snapshot;
  const Result<OwnerFolders> folders = owner_folders(user_sid_);
  if (!folders) {
    snapshot.problems.push_back("the owner's profile folders: " + folders.error().message);
    return snapshot;
  }
  for (const ChromiumBrowser& browser : chromium_browsers) {
    read_chromium(browser, folders->local_appdata / browser.user_data, snapshot);
    for (const std::string& id : forced_ids(browser.policy, user_sid_)) {
      snapshot.forced.emplace_back(browser.browser, id);
    }
  }
  read_firefox(folders->roaming / LR"(Mozilla\Firefox\Profiles)", snapshot);
  return snapshot;
}

}  // namespace myclean::platform
