#include "platform/fs/protected_locations.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <KnownFolders.h>
#include <ShlObj.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"

namespace myclean::platform {

Result<safety::ProtectedLocations> protected_locations(HANDLE owner) {
  std::optional<Error> error;
  const auto get = [&error](const KNOWNFOLDERID& id, HANDLE token) {
    wil::unique_cotaskmem_string text;
    const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, token, &text);
    if (FAILED(result)) {
      if (!error) {
        error = Error{.code = Errc::io_error,
                      .message = "SHGetKnownFolderPath failed: HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result))};
      }
      return std::filesystem::path{};
    }
    return std::filesystem::path{text.get()};
  };

  safety::ProtectedLocations locations{
      .windows = get(FOLDERID_Windows, nullptr),
      .profiles = get(FOLDERID_UserProfiles, nullptr),
      .owner_profile = get(FOLDERID_Profile, owner),
      .owner_known_folders = {get(FOLDERID_Documents, owner), get(FOLDERID_Desktop, owner),
                              get(FOLDERID_Pictures, owner), get(FOLDERID_Videos, owner),
                              get(FOLDERID_Music, owner)},
      .myclean = {get(FOLDERID_ProgramData, nullptr) / L"MyClean",
                  get(FOLDERID_ProgramFiles, nullptr) / L"MyClean"},
  };
  if (error) {
    return tl::unexpected(*std::move(error));
  }
  return locations;
}

}  // namespace myclean::platform
