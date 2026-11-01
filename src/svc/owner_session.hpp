#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "core/cleaning/service.hpp"
#include "core/model/result.hpp"

// What the service learns about the owner from Windows: the owner's logon session (environment,
// deny-list locations, Downloads) for cleaning, and whether a pipe client is the owner's tray.
namespace myclean::svc {

// The owner's context. When this process runs as the owner (console mode) it is this process's
// own environment and known folders; in the service it comes from the token of the owner's
// logon session (WTSQueryUserToken over the sessions; needs LocalSystem): the environment block
// of that token (CreateEnvironmentBlock), the deny-list locations and the Downloads folder.
// Fails with not_found while the owner is not logged on.
[[nodiscard]] Result<cleaning::OwnerContext> owner_context(std::string_view owner_sid);

// True when process `pid` runs the image `tray_image` (the same file, also when one of the paths
// goes through a junction), as the user `owner_sid`, in an interactive session (not session 0).
// False when any of it cannot be read.
[[nodiscard]] bool is_owner_tray(std::uint32_t pid, std::string_view owner_sid,
                                 const std::filesystem::path& tray_image);

}  // namespace myclean::svc
