#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/model/result.hpp"

namespace myclean::platform {

// %ProgramData% (machine state) and this account's %LOCALAPPDATA%.
[[nodiscard]] Result<std::filesystem::path> program_data_dir();
[[nodiscard]] Result<std::filesystem::path> local_app_data_dir();

// This executable's full path.
[[nodiscard]] Result<std::filesystem::path> executable_path();

// The user SID of whoever is logged on at the physical console. Fails with not_found when nobody
// is, access_denied without SeTcbPrivilege (only LocalSystem has it).
[[nodiscard]] Result<std::string> console_user_sid();

// The SID of the owner of a file or directory.
[[nodiscard]] Result<std::string> file_owner_sid(const std::filesystem::path& path);

// The security of the service's state folder, in SDDL: owned by Administrators, with a
// protected DACL that every file and folder below inherits: SYSTEM and Administrators full
// control, `owner_sid` read and execute only.
[[nodiscard]] std::string state_dir_sddl(std::string_view owner_sid);
// Applies state_dir_sddl to `dir` and resets everything below it to that owner and the
// inherited ACEs. Needs administrator rights (it sets the owner).
[[nodiscard]] Result<void> protect_state_dir(const std::filesystem::path& dir,
                                             std::string_view owner_sid);

}  // namespace myclean::platform
