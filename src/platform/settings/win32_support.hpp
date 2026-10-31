#pragma once

// Helpers shared by the settings adapters: UTF-8 <-> UTF-16 and Win32 errors as Results.

#include <cstddef>
#include <string>
#include <string_view>

#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform::settings_detail {

inline std::wstring widen(std::string_view text, UINT code_page = CP_UTF8) {
  if (text.empty()) {
    return {};
  }
  const int length =
      ::MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
  return result;
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

// `what` failed with Win32 error `error`, in the Errc it belongs to.
inline tl::unexpected<Error> win32_error(std::string_view what, DWORD error) {
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
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      code = Errc::already_exists;
      break;
    default:
      break;
  }
  return fail(code, std::string(what) + ": Win32 error " + std::to_string(error));
}

}  // namespace myclean::platform::settings_detail
