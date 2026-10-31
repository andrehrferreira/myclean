#include "platform/settings/registry.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace {

using placement::RegistryType;
using placement::RegistryValue;
using settings_detail::narrow;
using settings_detail::widen;
using settings_detail::win32_error;

constexpr std::size_t max_key_name = 256;  // 255 characters and the terminator

struct Location {
  HKEY root = nullptr;
  std::wstring path;
};

Result<Location> location_of(std::string_view key) {
  constexpr std::string_view user = "HKCU\\";
  constexpr std::string_view machine = "HKLM\\";
  if (key.starts_with(user)) {
    return Location{.root = HKEY_CURRENT_USER, .path = widen(key.substr(user.size()))};
  }
  if (key.starts_with(machine)) {
    return Location{.root = HKEY_LOCAL_MACHINE, .path = widen(key.substr(machine.size()))};
  }
  return fail(Errc::invalid_argument,
              R"(a registry key starts with HKCU\ or HKLM\, not ")" + std::string(key) + "\"");
}

// Key `key` opened for `access`; a null handle when it does not exist.
Result<wil::unique_hkey> open(std::string_view key, REGSAM access) {
  Result<Location> location = location_of(key);
  if (!location) {
    return tl::unexpected(location.error());
  }
  wil::unique_hkey handle;
  const LSTATUS status = ::RegOpenKeyExW(location->root, location->path.c_str(), 0,
                                         access | KEY_WOW64_64KEY, handle.put());
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return win32_error("opening " + std::string(key), static_cast<DWORD>(status));
  }
  return handle;
}

std::string describe(std::string_view key, std::string_view name) {
  return std::string(key) + "\\" + std::string(name);
}

}  // namespace

Result<std::optional<RegistryValue>> Win32Registry::read(std::string_view key,
                                                         std::string_view name) const {
  Result<Location> location = location_of(key);
  if (!location) {
    return tl::unexpected(location.error());
  }
  const std::wstring value_name = widen(name);
  constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_DWORD | RRF_NOEXPAND |
                          RRF_SUBKEY_WOW6464KEY;
  for (;;) {
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = ::RegGetValueW(location->root, location->path.c_str(), value_name.c_str(),
                                    flags, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
      return std::optional<RegistryValue>{};
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("reading " + describe(key, name), static_cast<DWORD>(status));
    }
    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1);
    size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    status = ::RegGetValueW(location->root, location->path.c_str(), value_name.c_str(), flags,
                            &type, buffer.data(), &size);
    if (status == ERROR_MORE_DATA) {
      continue;  // it grew between the two reads
    }
    if (status == ERROR_FILE_NOT_FOUND) {
      return std::optional<RegistryValue>{};
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("reading " + describe(key, name), static_cast<DWORD>(status));
    }
    if (type == REG_DWORD) {
      DWORD number = 0;
      std::memcpy(&number, buffer.data(), sizeof(number));
      return std::optional<RegistryValue>{
          RegistryValue{.text = std::to_string(number), .type = RegistryType::dword}};
    }
    return std::optional<RegistryValue>{RegistryValue{
        .text = narrow(std::wstring_view{buffer.data()}),
        .type = type == REG_EXPAND_SZ ? RegistryType::expandable : RegistryType::string}};
  }
}

Result<void> Win32Registry::write(std::string_view key, std::string_view name,
                                  const RegistryValue& value) {
  Result<Location> location = location_of(key);
  if (!location) {
    return tl::unexpected(location.error());
  }
  wil::unique_hkey handle;
  LSTATUS status =
      ::RegCreateKeyExW(location->root, location->path.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, handle.put(), nullptr);
  if (status != ERROR_SUCCESS) {
    return win32_error("creating " + std::string(key), static_cast<DWORD>(status));
  }
  const std::wstring value_name = widen(name);
  if (value.type == RegistryType::dword) {
    std::uint32_t number = 0;
    const std::string& text = value.text;
    const char* last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(text.data(), last, number);
    if (error != std::errc{} || end != last) {
      return fail(Errc::invalid_argument, "a DWORD is a decimal number, not \"" + text + "\"");
    }
    const DWORD data = number;
    status = ::RegSetKeyValueW(handle.get(), nullptr, value_name.c_str(), REG_DWORD, &data,
                               sizeof(data));
  } else {
    const std::wstring text = widen(value.text);
    status =
        ::RegSetKeyValueW(handle.get(), nullptr, value_name.c_str(),
                          value.type == RegistryType::expandable ? REG_EXPAND_SZ : REG_SZ,
                          text.c_str(), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
  }
  if (status != ERROR_SUCCESS) {
    return win32_error("writing " + describe(key, name), static_cast<DWORD>(status));
  }
  return {};
}

Result<void> Win32Registry::remove(std::string_view key, std::string_view name) {
  Result<wil::unique_hkey> handle = open(key, KEY_SET_VALUE);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  if (!*handle) {
    return {};
  }
  const LSTATUS status = ::RegDeleteValueW(handle->get(), widen(name).c_str());
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return win32_error("deleting " + describe(key, name), static_cast<DWORD>(status));
  }
  return {};
}

Result<std::vector<std::string>> Win32Registry::subkeys(std::string_view key) const {
  Result<wil::unique_hkey> handle = open(key, KEY_ENUMERATE_SUB_KEYS);
  if (!handle) {
    return tl::unexpected(handle.error());
  }
  std::vector<std::string> names;
  if (!*handle) {
    return names;
  }
  std::array<wchar_t, max_key_name> name{};
  for (DWORD index = 0;; ++index) {
    auto length = static_cast<DWORD>(name.size());
    const LSTATUS status = ::RegEnumKeyExW(handle->get(), index, name.data(), &length, nullptr,
                                           nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) {
      return names;
    }
    if (status != ERROR_SUCCESS) {
      return win32_error("listing the subkeys of " + std::string(key), static_cast<DWORD>(status));
    }
    names.push_back(narrow(std::wstring_view{name.data(), length}));
  }
}

}  // namespace myclean::platform
