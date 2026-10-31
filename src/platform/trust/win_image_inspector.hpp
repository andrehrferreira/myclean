#pragma once

#include <filesystem>
#include <vector>

#include "core/model/result.hpp"
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

// The folders a standard user can write to, for trust::classify_path: every user profile
// folder (with its AppData, Downloads and %TEMP%), including Public, and %SystemRoot%\Temp.
[[nodiscard]] Result<std::vector<std::filesystem::path>> user_writable_roots();

}  // namespace myclean::platform
