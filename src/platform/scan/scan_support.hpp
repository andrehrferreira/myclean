#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include "core/model/result.hpp"

// Helpers shared by the platform/scan sources.
namespace myclean::platform::scan_support {

[[nodiscard]] std::string utf8(const std::filesystem::path& path);
[[nodiscard]] std::string narrow(const std::wstring& text);
[[nodiscard]] std::wstring wide(const std::string& utf8_text);

// The "\\?\" form of an absolute drive or UNC path, so that MAX_PATH does not apply.
[[nodiscard]] Result<std::wstring> win32_path(const std::filesystem::path& path);

// `error` (a Win32 error code) as a Result error: "<action> <path>: Win32 error <n>".
[[nodiscard]] Error os_error(DWORD error, std::string_view action,
                             const std::filesystem::path& path);

// True when `attributes` mark a cloud file whose content may not be local.
[[nodiscard]] constexpr bool is_placeholder(DWORD attributes) {
  return (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                        FILE_ATTRIBUTE_OFFLINE)) != 0;
}

}  // namespace myclean::platform::scan_support
