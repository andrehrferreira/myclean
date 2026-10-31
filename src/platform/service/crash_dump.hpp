#pragma once

#include <cstddef>
#include <filesystem>

#include <windows.h>

#include "core/model/result.hpp"

namespace myclean::platform {

// Writes a minidump of this process (threads, stacks, referenced memory) into `dir`, which must
// exist, as <exe name>-<pid>-<UTC yyyyMMdd-HHmmss>.dmp. `exception` may be null.
[[nodiscard]] Result<std::filesystem::path> write_crash_dump(const std::filesystem::path& dir,
                                                             EXCEPTION_POINTERS* exception);

// Deletes all but the newest `keep` .dmp files in `dir`.
[[nodiscard]] Result<void> prune_crash_dumps(const std::filesystem::path& dir, std::size_t keep);

// From now on, an unhandled SEH exception or std::terminate writes a dump into `dir` before the
// process ends (best effort: the process is already failing). Creates `dir` and deletes all but
// the newest `keep - 1` dumps in it now, so a crash loop cannot fill the disk. Failures that
// bypass in-process handlers (__fastfail, stack overflow on some paths) leave no dump here.
[[nodiscard]] Result<void> install_crash_dumps(const std::filesystem::path& dir,
                                               std::size_t keep = 10);

}  // namespace myclean::platform
