#pragma once

#include <windows.h>

#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"

namespace myclean::platform {

// The deny-list locations of this machine, from the Windows known folders: %WINDIR%, the
// profiles folder, the owner's profile and Documents, Desktop, Pictures, Videos and Music
// (wherever they are redirected), and %ProgramData%\MyClean and %ProgramFiles%\MyClean.
// `owner` is the owner's access token; nullptr means the calling user, which in the service
// (LocalSystem) is not the owner, so the service passes the logged-on owner's token.
[[nodiscard]] Result<safety::ProtectedLocations> protected_locations(HANDLE owner);

}  // namespace myclean::platform
