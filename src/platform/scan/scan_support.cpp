#include "platform/scan/scan_support.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform::scan_support {

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int wide_length = static_cast<int>(text.size());
  const int length =
      ::WideCharToMultiByte(CP_UTF8, 0, text.data(), wide_length, nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), wide_length, result.data(), length, nullptr,
                        nullptr);
  return result;
}

std::wstring wide(const std::string& utf8_text) {
  if (utf8_text.empty()) {
    return {};
  }
  const int narrow_length = static_cast<int>(utf8_text.size());
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(), narrow_length, nullptr, 0);
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(), narrow_length, result.data(), length);
  return result;
}

Result<std::wstring> win32_path(const std::filesystem::path& path) {
  const std::wstring& text = path.native();
  if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\") || !path.is_absolute()) {
    return fail(Errc::invalid_argument, "not an absolute drive or UNC path: " + utf8(path));
  }
  const std::filesystem::path normal = path.lexically_normal().make_preferred();
  if (normal.root_name().native().starts_with(L"\\\\")) {
    return L"\\\\?\\UNC\\" + normal.native().substr(2);
  }
  return L"\\\\?\\" + normal.native();
}

Error os_error(DWORD error, std::string_view action, const std::filesystem::path& path) {
  Errc code = Errc::io_error;
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
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
    case ERROR_DIRECTORY:
    case ERROR_INVALID_NAME:
      code = Errc::invalid_argument;
      break;
    default:
      break;
  }
  return Error{
      .code = code,
      .message = std::string(action) + " " + utf8(path) + ": Win32 error " + std::to_string(error)};
}

}  // namespace myclean::platform::scan_support
