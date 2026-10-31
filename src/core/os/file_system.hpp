#pragma once

#include <filesystem>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::os {

enum class EntryKind { file, directory };

struct FileAttributes {
  bool reparse_point = false;          // junction, symbolic link or other reparse point
  bool recall_on_data_access = false;  // cloud file: reading the content downloads it
  bool recall_on_open = false;         // cloud file: opening it downloads it
  bool offline = false;                // content is not stored locally

  // True when touching the content could hydrate a cloud file.
  [[nodiscard]] bool cloud_placeholder() const {
    return recall_on_data_access || recall_on_open || offline;
  }
};

// Metadata of one entry itself. A reparse point is described, never followed.
struct FileStat {
  EntryKind kind = EntryKind::file;
  FileAttributes attributes;
  FileIdentity identity;
  Bytes size;          // logical size; zero for directories
  Bytes size_on_disk;  // allocated size; zero for a cloud placeholder without local content
};

struct DirEntry {
  std::filesystem::path path;
  FileStat stat;
};

// The file system as core sees it. Implemented by src/platform/fs and by tests/fakes.
//
// Every path is absolute (drive letter or UNC); relative paths fail with invalid_argument.
// No operation follows a reparse point or reads file content, so none can hydrate a cloud file.
class IFileSystem {
 public:
  virtual ~IFileSystem() = default;
  IFileSystem(const IFileSystem&) = delete;
  IFileSystem& operator=(const IFileSystem&) = delete;
  IFileSystem(IFileSystem&&) = delete;
  IFileSystem& operator=(IFileSystem&&) = delete;

  // Metadata of `path` itself.
  [[nodiscard]] virtual Result<FileStat> stat(const std::filesystem::path& path) const = 0;

  // The entries directly inside directory `path`, without recursion. Fails with
  // invalid_argument when `path` is a reparse point, so no caller can list through a link.
  [[nodiscard]] virtual Result<std::vector<DirEntry>> enumerate(
      const std::filesystem::path& path) const = 0;

  // The long form of an existing path: every 8.3 short component (PROGRA~1) expanded.
  [[nodiscard]] virtual Result<std::filesystem::path> expand_short_names(
      const std::filesystem::path& path) const = 0;

  // Removes a file, an empty directory, or a reparse point as a link (never its target).
  // Does not recurse: fails with not_empty for a directory that has entries.
  [[nodiscard]] virtual Result<void> remove(const std::filesystem::path& path) = 0;

  // Renames a file, directory or reparse point atomically on one volume; identity is kept.
  // Fails with already_exists when `to` exists and invalid_argument across volumes.
  [[nodiscard]] virtual Result<void> rename(const std::filesystem::path& from,
                                            const std::filesystem::path& to) = 0;

  // Moves one regular file, also to another volume (copy, then delete the source). Fails with
  // already_exists when `to` exists and invalid_argument when `from` is not a regular file.
  [[nodiscard]] virtual Result<void> move_file(const std::filesystem::path& from,
                                               const std::filesystem::path& to) = 0;

  // Creates `path` and its missing parents; succeeds when it is already a directory.
  [[nodiscard]] virtual Result<void> create_directories(const std::filesystem::path& path) = 0;

 protected:
  IFileSystem() = default;
};

}  // namespace myclean::os
