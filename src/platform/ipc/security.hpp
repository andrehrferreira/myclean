#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {

inline constexpr std::string_view system_sid = "S-1-5-18";
inline constexpr std::string_view administrators_sid = "S-1-5-32-544";

// What the owner may do on the service pipe: read and write messages and attributes, but not
// FILE_CREATE_PIPE_INSTANCE, so no client can stand up a server instance of its own.
inline constexpr DWORD file_generic_read = FILE_GENERIC_READ;
inline constexpr DWORD file_generic_write = FILE_GENERIC_WRITE;
inline constexpr DWORD pipe_client_access =
    (file_generic_read | file_generic_write) & ~static_cast<DWORD>(FILE_CREATE_PIPE_INSTANCE);

// The string form (S-1-...) of the user SID of `token`.
[[nodiscard]] Result<std::string> token_user_sid(HANDLE token);
// The user SID of this process.
[[nodiscard]] Result<std::string> current_user_sid();
// Fails with invalid_argument unless `sid` is a well-formed string SID.
[[nodiscard]] Result<void> check_sid(std::string_view sid);

// The DACL of the service pipe, in SDDL: protected (nothing inherited), one ACE granting SYSTEM
// full access and one granting `owner_sid` pipe_client_access, or full access when the owner
// is the account that runs the server (console mode), since the server must create instances.
[[nodiscard]] std::string pipe_sddl(std::string_view owner_sid, bool owner_is_server);

// A self-relative security descriptor parsed from SDDL.
[[nodiscard]] Result<wil::unique_hlocal_security_descriptor> make_security_descriptor(
    std::string_view sddl);

// One access-allowed ACE of a DACL, for checks and tests.
struct AllowedAce {
  std::string sid;
  DWORD mask = 0;
  BYTE flags = 0;
};

// The ACEs of `descriptor`'s DACL, in order. Fails with invalid_data when the DACL is missing
// (which would grant everyone access) or holds an ACE that is not access-allowed.
[[nodiscard]] Result<std::vector<AllowedAce>> allowed_aces(PSECURITY_DESCRIPTOR descriptor);
// The same for the DACL of an open kernel object (pipe, file, directory).
[[nodiscard]] Result<std::vector<AllowedAce>> object_aces(HANDLE object);

}  // namespace myclean::platform
