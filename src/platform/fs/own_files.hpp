#pragma once

#include <filesystem>

#include "core/model/result.hpp"
#include "core/safety/deny_list.hpp"

namespace myclean::platform {

// Deletes one regular file that MyClean created itself, such as a crash dump, the staging file of
// a settings write, or a config file it wrote. These deletes are the deliberate exceptions to
// SafeRemove (architecture section 6), so this helper applies the same guards to them:
//   - `path` must pass safety::normalize_path;
//   - every deny-list entry applies but `myclean`, which keeps cleaning rules out of MyClean's
//     own folders, not MyClean's own upkeep of them;
//   - the file and every folder above it must not be a reparse point, and the file must not be
//     a directory. The file is opened without following a reparse point and deleted through that
//     handle, so a link swapped in after the check is not followed either.
// Fails with refused on a guard, and succeeds when the file does not exist.
[[nodiscard]] Result<void> remove_own_file(const std::filesystem::path& path,
                                           const safety::ProtectedLocations& locations);

}  // namespace myclean::platform
