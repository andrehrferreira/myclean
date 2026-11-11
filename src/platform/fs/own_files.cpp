#include "platform/fs/own_files.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"

namespace myclean::platform {

namespace {

namespace fs = std::filesystem;

// The deny-list entry that `path` falls under, leaving out `myclean`.
std::optional<std::string_view> denied(const fs::path& path,
                                       const safety::ProtectedLocations& locations) {
  for (const safety::DenyRule& rule : safety::deny_list()) {
    if (rule.name != "myclean" && rule.matches(path, locations)) {
      return rule.name;
    }
  }
  return std::nullopt;
}

bool gone(DWORD error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

Errc errc_of(DWORD error) {
  if (error == ERROR_ACCESS_DENIED) {
    return Errc::access_denied;
  }
  if (error == ERROR_SHARING_VIOLATION) {
    return Errc::in_use;
  }
  return Errc::io_error;
}

tl::unexpected<Error> os_error(DWORD error, std::string_view action, const fs::path& path) {
  return fail(errc_of(error), std::string(action) + " " + safety::to_utf8(path) + ": Win32 error " +
                                  std::to_string(error));
}

}  // namespace

Result<void> remove_own_file(const fs::path& path, const safety::ProtectedLocations& locations) {
  Result<fs::path> normalized = safety::normalize_path(path);
  if (!normalized) {
    return tl::unexpected(normalized.error());
  }
  if (const std::optional<std::string_view> rule = denied(*normalized, locations)) {
    return fail(Errc::refused, "deny_list:" + std::string(*rule) + ": " + safety::to_utf8(path));
  }
  // The folders above it, so that no junction leads the delete somewhere else.
  for (fs::path folder = normalized->parent_path(); folder.has_relative_path();
       folder = folder.parent_path()) {
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      if (gone(error)) {
        return {};
      }
      return os_error(error, "reading the attributes of", folder);
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(Errc::refused, "a reparse point leads to " + safety::to_utf8(path));
    }
  }
  const wil::unique_hfile file{
      ::CreateFileW(normalized->c_str(), DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  if (!file) {
    const DWORD error = ::GetLastError();
    if (gone(error)) {
      return {};
    }
    return os_error(error, "opening", path);
  }
  FILE_BASIC_INFO basic{};
  if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
    return os_error(::GetLastError(), "reading the attributes of", path);
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(Errc::refused,
                "a reparse point, not a file of MyClean's: " + safety::to_utf8(path));
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(Errc::refused, "a folder, not a file: " + safety::to_utf8(path));
  }
  FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
  if (::SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition,
                                   sizeof(disposition)) == FALSE) {
    return os_error(::GetLastError(), "deleting", path);
  }
  return {};
}

}  // namespace myclean::platform
