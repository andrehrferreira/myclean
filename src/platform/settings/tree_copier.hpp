#pragma once

#include <filesystem>

#include "core/model/result.hpp"
#include "core/placement/settings.hpp"

namespace myclean::platform {

// Copies a folder tree with FindFirstFileExW and CopyFileExW (COPY_FILE_FAIL_IF_EXISTS), through
// "\\?\" paths so that deep cache trees work. Reparse points are skipped, never followed.
class Win32TreeCopier final : public placement::ITreeCopier {
 public:
  Win32TreeCopier() = default;

  [[nodiscard]] Result<placement::TreeTotals> copy_tree(const std::filesystem::path& from,
                                                        const std::filesystem::path& to) override;
};

}  // namespace myclean::platform
