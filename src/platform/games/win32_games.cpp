#include "platform/games/win32_games.hpp"

// clang-format off
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <wil/resource.h>

#include "core/games/fullscreen.hpp"
#include "core/games/game_config_store.hpp"
#include "core/games/library_roots.hpp"
#include "core/games/paths.hpp"
#include "core/games/steam.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view steam_key = LR"(Software\Valve\Steam)";
constexpr std::wstring_view game_config_key = LR"(System\GameConfigStore\Children)";
constexpr std::wstring_view gog_key = LR"(SOFTWARE\WOW6432Node\GOG.com\Games)";
constexpr std::size_t max_file_bytes = std::size_t{4} * 1024 * 1024;  // a manifest is a few KB

constexpr DWORD placeholder_attributes =
    FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
  return result;
}

Error win32_error(std::string_view what, DWORD code) {
  Errc errc = Errc::io_error;
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    errc = Errc::not_found;
  } else if (code == ERROR_ACCESS_DENIED) {
    errc = Errc::access_denied;
  }
  return Error{
      .code = errc,
      .message = std::string(what) + ": " + std::system_category().message(static_cast<int>(code))};
}

bool missing(DWORD code) {
  return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

// Opens `root\path` for reading; the Win32 error code on failure.
LSTATUS open_key(HKEY root, const std::wstring& path, wil::unique_hkey& key) {
  return ::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, key.put());
}

std::vector<std::wstring> subkeys(HKEY key) {
  std::vector<std::wstring> names;
  std::array<wchar_t, 256> name{};  // registry key names are at most 255 characters
  for (DWORD index = 0;; ++index) {
    auto length = static_cast<DWORD>(name.size());
    const LSTATUS status =
        ::RegEnumKeyExW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS || (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)) {
      break;
    }
    if (status == ERROR_SUCCESS) {
      names.emplace_back(name.data(), length);
    }
  }
  return names;
}

// The REG_SZ value `name` of `key\subkey`; nullopt when it is missing or of another type.
std::optional<std::wstring> string_value(HKEY key, const std::wstring& subkey,
                                         const wchar_t* name) {
  DWORD bytes = 0;
  if (::RegGetValueW(key, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
          ERROR_SUCCESS ||
      bytes < sizeof(wchar_t)) {
    return std::nullopt;
  }
  std::wstring text(bytes / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(key, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  text.resize(bytes / sizeof(wchar_t));
  while (!text.empty() && text.back() == L'\0') {
    text.pop_back();
  }
  return text;
}

std::optional<std::uint32_t> dword_value(HKEY key, const std::wstring& subkey,
                                         const wchar_t* name) {
  DWORD value = 0;
  DWORD bytes = sizeof(value);
  if (::RegGetValueW(key, subkey.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

// Reads a small regular file. Opened as the entry itself (a link is refused, not followed) and
// without recall, so a cloud placeholder is refused instead of downloaded.
Result<std::string> read_file(const fs::path& path) {
  const wil::unique_hfile file{::CreateFileW(
      path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr)};
  const std::string name = games::utf8(path);
  if (!file) {
    return tl::unexpected(win32_error(name, ::GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  if (::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ==
          FALSE ||
      ::GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)) ==
          FALSE) {
    return tl::unexpected(win32_error(name, ::GetLastError()));
  }
  if ((tag.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | placeholder_attributes)) != 0) {
    return fail(Errc::refused, name + ": a folder, link or cloud file, not read");
  }
  if (standard.EndOfFile.QuadPart < 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) > max_file_bytes) {
    return fail(Errc::invalid_data, name + ": larger than 4 MB, not read");
  }
  std::string text(static_cast<std::size_t>(standard.EndOfFile.QuadPart), '\0');
  std::size_t done = 0;
  while (done < text.size()) {
    DWORD read = 0;
    if (::ReadFile(file.get(), &text[done], static_cast<DWORD>(text.size() - done), &read,
                   nullptr) == FALSE) {
      return tl::unexpected(win32_error(name, ::GetLastError()));
    }
    if (read == 0) {
      break;
    }
    done += read;
  }
  text.resize(done);
  constexpr std::string_view bom = "\xEF\xBB\xBF";
  if (text.starts_with(bom)) {
    text.erase(0, bom.size());
  }
  return text;
}

std::optional<DWORD> attributes_of(const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
    return std::nullopt;
  }
  return data.dwFileAttributes;
}

// The entries of `folder` matching `pattern`, as listed (FindFirstFileEx: no entry is opened).
std::vector<WIN32_FIND_DATAW> list(const fs::path& folder, const wchar_t* pattern,
                                   FINDEX_SEARCH_OPS search) {
  std::vector<WIN32_FIND_DATAW> entries;
  WIN32_FIND_DATAW data{};
  const wil::unique_hfind find{::FindFirstFileExW((folder / pattern).c_str(), FindExInfoBasic,
                                                  &data, search, nullptr,
                                                  FIND_FIRST_EX_LARGE_FETCH)};
  for (bool more = static_cast<bool>(find); more;
       more = ::FindNextFileW(find.get(), &data) != FALSE) {
    const std::wstring_view name{static_cast<const wchar_t*>(data.cFileName)};
    if (name != L"." && name != L"..") {
      entries.push_back(data);
    }
  }
  return entries;
}

void read_steam(const std::optional<fs::path>& steam, games::LauncherData& data) {
  if (!steam) {
    return;
  }
  Result<std::string> text = read_file(games::library_folders_file(*steam));
  if (text) {
    data.steam_library_files.push_back(std::move(*text));
  } else if (text.error().code != Errc::not_found) {
    data.problems.push_back("Steam: " + text.error().message);
  }
}

void read_epic(games::LauncherData& data) {
  wil::unique_cotaskmem_string program_data;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr,
                                    program_data.put()))) {
    data.problems.emplace_back("Epic: the ProgramData folder is unknown");
    return;
  }
  const fs::path folder =
      fs::path{program_data.get()} / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
  const std::optional<DWORD> attributes = attributes_of(folder);
  if (!attributes) {
    return;  // the launcher is not installed
  }
  if ((*attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    data.problems.push_back("Epic: " + games::utf8(folder) + " is a link, not followed");
    return;
  }
  for (const WIN32_FIND_DATAW& entry : list(folder, L"*.item", FindExSearchNameMatch)) {
    const fs::path file = folder / static_cast<const wchar_t*>(entry.cFileName);
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !games::same_path(file.extension(), L".item")) {
      continue;
    }
    Result<std::string> text = read_file(file);
    if (text) {
      data.epic_manifests.push_back(std::move(*text));
    } else {
      data.problems.push_back("Epic: " + text.error().message);
    }
  }
}

void read_gog(games::LauncherData& data) {
  wil::unique_hkey games_key;
  if (const LSTATUS status = open_key(HKEY_LOCAL_MACHINE, std::wstring(gog_key), games_key);
      status != ERROR_SUCCESS) {
    if (!missing(static_cast<DWORD>(status))) {
      data.problems.push_back(win32_error("GOG", static_cast<DWORD>(status)).message);
    }
    return;
  }
  for (const std::wstring& game : subkeys(games_key.get())) {
    if (std::optional<std::wstring> path = string_value(games_key.get(), game, L"path");
        path && !path->empty()) {
      data.gog_folders.push_back(fs::path{*path}.make_preferred());
    }
  }
}

}  // namespace

std::optional<fs::path> Win32GameSources::steam_path() const {
  wil::unique_hkey key;
  const LSTATUS status =
      owner_sid_.empty() ? open_key(HKEY_CURRENT_USER, std::wstring(steam_key), key)
                         : open_key(HKEY_USERS, owner_sid_ + L"\\" + std::wstring(steam_key), key);
  if (status != ERROR_SUCCESS) {
    return std::nullopt;
  }
  std::optional<std::wstring> path = string_value(key.get(), L"", L"SteamPath");
  if (!path || path->empty()) {
    return std::nullopt;
  }
  return fs::path{*path}.make_preferred();
}

games::LauncherData Win32GameSources::launcher_data() const {
  games::LauncherData data;
  read_steam(steam_path(), data);
  read_epic(data);
  read_gog(data);
  data.drive_roots = fixed_drive_roots();
  return data;
}

Result<std::vector<games::GameConfigEntry>> Win32GameSources::game_config_store() const {
  std::vector<games::GameConfigEntry> entries;
  wil::unique_hkey children;
  const LSTATUS status =
      owner_sid_.empty()
          ? open_key(HKEY_CURRENT_USER, std::wstring(game_config_key), children)
          : open_key(HKEY_USERS, owner_sid_ + L"\\" + std::wstring(game_config_key), children);
  if (missing(static_cast<DWORD>(status))) {
    return entries;
  }
  if (status != ERROR_SUCCESS) {
    return tl::unexpected(win32_error("GameConfigStore", static_cast<DWORD>(status)));
  }
  for (const std::wstring& child : subkeys(children.get())) {
    games::GameConfigEntry entry{.type = dword_value(children.get(), child, L"Type").value_or(0)};
    if (std::optional<std::wstring> path =
            string_value(children.get(), child, L"MatchedExeFullPath")) {
      entry.matched_exe_full_path = fs::path{*path};
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

bool Win32GameFiles::is_directory(const fs::path& path) const {
  const std::optional<DWORD> attributes = attributes_of(path);
  return attributes && (*attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool Win32GameFiles::is_file(const fs::path& path) const {
  const std::optional<DWORD> attributes = attributes_of(path);
  return attributes && (*attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<fs::path> Win32GameFiles::subfolders(const fs::path& path) const {
  std::vector<fs::path> folders;
  const std::optional<DWORD> attributes = attributes_of(path);
  if (!attributes || (*attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (*attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return folders;
  }
  for (const WIN32_FIND_DATAW& entry : list(path, L"*", FindExSearchLimitToDirectories)) {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      folders.push_back(path / static_cast<const wchar_t*>(entry.cFileName));
    }
  }
  return folders;
}

std::vector<fs::path> fixed_drive_roots() {
  std::vector<fs::path> roots;
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((drives & (1U << static_cast<unsigned>(letter - L'A'))) == 0) {
      continue;
    }
    const std::wstring root{letter, L':', L'\\'};
    if (::GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
      roots.emplace_back(root);
    }
  }
  return roots;
}

Result<games::NotificationState> query_user_notification_state() {
  QUERY_USER_NOTIFICATION_STATE state{};
  if (const HRESULT result = ::SHQueryUserNotificationState(&state); FAILED(result)) {
    return fail(Errc::io_error, "SHQueryUserNotificationState: " +
                                    std::system_category().message(static_cast<int>(result)));
  }
  switch (state) {
    case QUNS_NOT_PRESENT:
      return games::NotificationState::not_present;
    case QUNS_BUSY:
      return games::NotificationState::busy;
    case QUNS_RUNNING_D3D_FULL_SCREEN:
      return games::NotificationState::running_d3d_full_screen;
    case QUNS_PRESENTATION_MODE:
      return games::NotificationState::presentation_mode;
    case QUNS_ACCEPTS_NOTIFICATIONS:
      return games::NotificationState::accepts_notifications;
    case QUNS_QUIET_TIME:
      return games::NotificationState::quiet_time;
    case QUNS_APP:
      return games::NotificationState::app;
  }
  return fail(Errc::invalid_data, "SHQueryUserNotificationState returned unknown state " +
                                      std::to_string(static_cast<int>(state)));
}

}  // namespace myclean::platform
