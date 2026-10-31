#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform::defrag_detail {

inline std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

inline std::string narrow(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

// "<action> <path>: Win32 error <n>" in the Errc the error belongs to.
inline tl::unexpected<Error> win32_error(std::string_view action, const std::filesystem::path& path,
                                         DWORD error) {
  Errc code = Errc::io_error;
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      code = Errc::not_found;
      break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
      code = Errc::access_denied;
      break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      code = Errc::in_use;
      break;
    default:
      break;
  }
  return fail(code,
              std::string(action) + " " + utf8(path) + ": Win32 error " + std::to_string(error));
}

}  // namespace myclean::platform::defrag_detail
