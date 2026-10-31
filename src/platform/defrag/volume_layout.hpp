#pragma once

#include <chrono>
#include <filesystem>
#include <vector>

#include "core/drives/layout.hpp"
#include "core/model/result.hpp"

namespace myclean::platform {

// drives::IVolumeLayout over the NTFS defragmentation interface, read-only:
//   extents     FSCTL_GET_RETRIEVAL_POINTERS on the file opened with FILE_READ_ATTRIBUTES only
//               (no read access, reparse points not followed); works without elevation
//   free_space  FSCTL_GET_VOLUME_BITMAP in 1 MiB pieces on the volume device; opening the
//               volume needs administrator rights, so it fails with access_denied otherwise
class Win32VolumeLayout final : public drives::IVolumeLayout {
 public:
  Win32VolumeLayout() = default;

  [[nodiscard]] Result<std::vector<drives::Extent>> extents(
      const std::filesystem::path& file) const override;
  [[nodiscard]] Result<drives::FreeSpaceLayout> free_space(
      const std::filesystem::path& root, std::chrono::milliseconds budget) const override;
};

// Lowers the calling thread to background mode (low CPU and I/O priority) for its lifetime;
// the fragmentation sample and bitmap read run under it.
class BackgroundPriority {
 public:
  BackgroundPriority();
  ~BackgroundPriority();
  BackgroundPriority(const BackgroundPriority&) = delete;
  BackgroundPriority& operator=(const BackgroundPriority&) = delete;
  BackgroundPriority(BackgroundPriority&&) = delete;
  BackgroundPriority& operator=(BackgroundPriority&&) = delete;

  // False when Windows refused (the thread then runs at its normal priority).
  [[nodiscard]] bool active() const { return active_; }

 private:
  bool active_ = false;
};

}  // namespace myclean::platform
