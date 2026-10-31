#pragma once

#include <cstddef>
#include <string>

#include <windows.h>

// UTF-8 and UTF-16 conversions of the cleanup module.
namespace myclean::platform {

[[nodiscard]] inline std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length = static_cast<int>(text.size());
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), size);
  return wide;
}

[[nodiscard]] inline std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int length = static_cast<int>(text.size());
  const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, bytes.data(), size, nullptr, nullptr);
  return bytes;
}

}  // namespace myclean::platform
