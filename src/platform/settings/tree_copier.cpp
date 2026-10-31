#include "platform/settings/tree_copier.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/settings.hpp"
#include "platform/settings/win32_support.hpp"

namespace myclean::platform {

namespace {

using settings_detail::narrow;
using settings_detail::win32_error;

// `path` in the "\\?\" form that lifts the MAX_PATH limit.
std::wstring extended(const std::filesystem::path& path) {
  const std::wstring& text = path.native();
  return text.starts_with(LR"(\\?\)") ? text : LR"(\\?\)" + text;
}

Result<void> create_folder(const std::wstring& folder) {
  if (::CreateDirectoryW(folder.c_str(), nullptr) == FALSE) {
    if (const DWORD error = ::GetLastError(); error != ERROR_ALREADY_EXISTS) {
      return win32_error("creating " + narrow(folder), error);
    }
  }
  return {};
}

}  // namespace

Result<placement::TreeTotals> Win32TreeCopier::copy_tree(const std::filesystem::path& from,
                                                         const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::create_directories(to, error);
  if (error) {
    return fail(Errc::io_error, "creating " + narrow(to.native()) + ": " + error.message());
  }
  placement::TreeTotals totals;
  std::vector<std::pair<std::wstring, std::wstring>> folders{{extended(from), extended(to)}};
  while (!folders.empty()) {
    const auto [source, destination] = std::move(folders.back());
    folders.pop_back();
    WIN32_FIND_DATAW entry{};
    const wil::unique_hfind find{::FindFirstFileExW((source + L"\\*").c_str(), FindExInfoBasic,
                                                    &entry, FindExSearchNameMatch, nullptr,
                                                    FIND_FIRST_EX_LARGE_FETCH)};
    if (!find) {
      return win32_error("listing " + narrow(source), ::GetLastError());
    }
    for (bool more = true; more; more = ::FindNextFileW(find.get(), &entry) != FALSE) {
      const std::wstring name{static_cast<const wchar_t*>(entry.cFileName)};
      if (name == L"." || name == L".." ||
          (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        continue;
      }
      std::wstring inner = source;
      inner.append(L"\\").append(name);
      std::wstring outer = destination;
      outer.append(L"\\").append(name);
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (Result<void> created = create_folder(outer); !created) {
          return tl::unexpected(created.error());
        }
        folders.emplace_back(std::move(inner), std::move(outer));
        continue;
      }
      if (::CopyFileExW(inner.c_str(), outer.c_str(), nullptr, nullptr, nullptr,
                        COPY_FILE_FAIL_IF_EXISTS) == FALSE) {
        return win32_error("copying " + narrow(inner) + " to " + narrow(outer), ::GetLastError());
      }
      ++totals.files;
      totals.bytes +=
          Bytes{(static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32U) | entry.nFileSizeLow};
    }
    if (const DWORD last = ::GetLastError(); last != ERROR_NO_MORE_FILES) {
      return win32_error("listing " + narrow(source), last);
    }
  }
  return totals;
}

}  // namespace myclean::platform
