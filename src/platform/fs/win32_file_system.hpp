#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"

namespace myclean::platform {

// IFileSystem over Win32. Every entry is opened with FILE_FLAG_OPEN_REPARSE_POINT and
// FILE_FLAG_BACKUP_SEMANTICS, so a junction or symbolic link is described and removed as the
// link itself and never followed. Only copy_file reads file content, after it has refused a
// cloud placeholder, so no call hydrates a cloud file.
// Paths are passed to Win32 in their "\\?\" form, so they may exceed MAX_PATH. Identities need
// file ids (NTFS, ReFS); FAT volumes fail with io_error.
class Win32FileSystem final : public os::IFileSystem {
 public:
  Win32FileSystem() = default;

  [[nodiscard]] Result<os::FileStat> stat(const std::filesystem::path& path) const override;
  [[nodiscard]] Result<std::vector<os::DirEntry>> enumerate(
      const std::filesystem::path& path) const override;
  [[nodiscard]] Result<std::filesystem::path> expand_short_names(
      const std::filesystem::path& path) const override;
  [[nodiscard]] Result<void> remove(const std::filesystem::path& path) override;
  [[nodiscard]] Result<void> remove(const std::filesystem::path& path,
                                    const FileIdentity& expected) override;
  [[nodiscard]] Result<void> rename(const std::filesystem::path& from,
                                    const std::filesystem::path& to) override;
  [[nodiscard]] Result<void> move_file(const std::filesystem::path& from,
                                       const std::filesystem::path& to) override;
  [[nodiscard]] Result<os::FileStat> copy_file(const std::filesystem::path& from,
                                               const std::filesystem::path& to,
                                               const FileIdentity& expected) override;
  [[nodiscard]] Result<void> flush(std::span<const std::filesystem::path> files) override;
  [[nodiscard]] Result<void> create_directories(const std::filesystem::path& path) override;
};

}  // namespace myclean::platform
