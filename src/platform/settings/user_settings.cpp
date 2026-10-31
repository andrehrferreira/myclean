#include "platform/settings/user_settings.hpp"

#include <bit>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace {

using settings_detail::narrow;
using settings_detail::widen;
using settings_detail::win32_error;

constexpr const wchar_t* environment_key = L"Environment";
constexpr UINT broadcast_timeout_ms = 5000;

}  // namespace

Result<std::optional<placement::EnvValue>> Win32UserEnvironment::get(std::string_view name) const {
  const std::wstring value_name = widen(name);
  constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
  for (;;) {
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, environment_key, value_name.c_str(), flags,
                                    &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
      return std::optional<placement::EnvValue>{};
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("reading user variable " + std::string(name), static_cast<DWORD>(status));
    }
    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1);
    size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    status = ::RegGetValueW(HKEY_CURRENT_USER, environment_key, value_name.c_str(), flags, &type,
                            buffer.data(), &size);
    if (status == ERROR_MORE_DATA) {
      continue;  // it grew between the two reads
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("reading user variable " + std::string(name), static_cast<DWORD>(status));
    }
    return std::optional<placement::EnvValue>{placement::EnvValue{
        .text = narrow(std::wstring_view{buffer.data()}), .expandable = type == REG_EXPAND_SZ}};
  }
}

Result<void> Win32UserEnvironment::set(std::string_view name, const placement::EnvValue& value) {
  const std::wstring text = widen(value.text);
  const LSTATUS status = ::RegSetKeyValueW(HKEY_CURRENT_USER, environment_key, widen(name).c_str(),
                                           value.expandable ? REG_EXPAND_SZ : REG_SZ, text.c_str(),
                                           static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
  if (status != ERROR_SUCCESS) {
    return win32_error("writing user variable " + std::string(name), static_cast<DWORD>(status));
  }
  return {};
}

Result<void> Win32UserEnvironment::remove(std::string_view name) {
  const LSTATUS status =
      ::RegDeleteKeyValueW(HKEY_CURRENT_USER, environment_key, widen(name).c_str());
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return win32_error("deleting user variable " + std::string(name), static_cast<DWORD>(status));
  }
  return {};
}

Result<std::filesystem::path> Win32UserEnvironment::expand(std::string_view text) const {
  const std::wstring source = widen(text);
  std::wstring expanded(source.size() + 1, L'\0');
  for (;;) {
    const DWORD needed = ::ExpandEnvironmentStringsW(source.c_str(), expanded.data(),
                                                     static_cast<DWORD>(expanded.size()));
    if (needed == 0) {
      return win32_error("expanding \"" + std::string(text) + "\"", ::GetLastError());
    }
    if (needed <= expanded.size()) {
      expanded.resize(needed - 1);
      return std::filesystem::path{expanded};
    }
    expanded.resize(needed);
  }
}

Result<void> Win32UserEnvironment::announce() {
  const auto area = std::bit_cast<LPARAM>(environment_key);  // the message's string parameter
  DWORD_PTR result = 0;
  if (::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, area, SMTO_ABORTIFHUNG,
                            broadcast_timeout_ms, &result) == 0) {
    return win32_error("broadcasting the environment change", ::GetLastError());
  }
  return {};
}

Result<std::optional<std::string>> Win32TextFiles::read(const std::filesystem::path& path) const {
  const wil::unique_hfile file{::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!file) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return std::optional<std::string>{};
    }
    return win32_error("opening " + narrow(path.native()), error);
  }
  std::string text;
  std::vector<char> buffer(std::size_t{64} * 1024);
  for (;;) {
    DWORD read = 0;
    if (::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
        FALSE) {
      return win32_error("reading " + narrow(path.native()), ::GetLastError());
    }
    if (read == 0) {
      return std::optional<std::string>{std::move(text)};
    }
    text.append(buffer.data(), read);
  }
}

Result<void> Win32TextFiles::write(const std::filesystem::path& path, std::string_view text) {
  const std::wstring staging = path.native() + L".myclean-new";
  {
    const wil::unique_hfile file{::CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) {
      return win32_error("creating " + narrow(staging), ::GetLastError());
    }
    DWORD written = 0;
    if (::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ==
            FALSE ||
        written != text.size() || ::FlushFileBuffers(file.get()) == FALSE) {
      const DWORD error = ::GetLastError();
      ::DeleteFileW(staging.c_str());
      return win32_error("writing " + narrow(staging), error);
    }
  }
  if (::MoveFileExW(staging.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    const DWORD error = ::GetLastError();
    ::DeleteFileW(staging.c_str());
    return win32_error("replacing " + narrow(path.native()), error);
  }
  return {};
}

Result<void> Win32TextFiles::remove(const std::filesystem::path& path) {
  if (::DeleteFileW(path.c_str()) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      return win32_error("deleting " + narrow(path.native()), error);
    }
  }
  return {};
}

}  // namespace myclean::platform
