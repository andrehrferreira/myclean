#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/trust/trust.hpp"

namespace myclean::platform {

// Authenticode through WinVerifyTrust: the embedded signature first, then the Windows catalogs
// (SHA-256, then SHA-1 hashes), which sign most files of Windows itself. Revocation is not
// checked and certificates are taken from the local caches only, so a check never waits on the
// network. Thread-safe.
class WinImageInspector final : public trust::IImageInspector {
 public:
  WinImageInspector() = default;

  [[nodiscard]] Result<trust::FileStamp> stamp(const std::filesystem::path& image) const override;
  [[nodiscard]] Result<trust::Signature> verify(const std::filesystem::path& image) const override;
};

// Image paths of running processes: QueryFullProcessImageName on a handle opened with
// PROCESS_QUERY_LIMITED_INFORMATION, accepted only while the process still has the expected
// creation time, so a reused PID never gives another process's path. Thread-safe.
class WinProcessImages final : public trust::IProcessImages {
 public:
  WinProcessImages() = default;

  [[nodiscard]] Result<std::filesystem::path> image_path(std::uint32_t pid,
                                                         WallTime created) const override;
};

// The folders a standard user can write to, for trust::classify_path: every user profile
// folder (with its AppData, Downloads and %TEMP%), including Public, and %SystemRoot%\Temp.
[[nodiscard]] Result<std::vector<std::filesystem::path>> user_writable_roots();

}  // namespace myclean::platform
