#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

// What the cleaning rule engine reads from the machine. Every interface is read-only: nothing
// here deletes, moves, writes or reads file content. Implemented by src/platform/clean and
// src/platform/git, and by in-memory fakes in tests.
namespace myclean::clean {

// One file system entry with the time it last changed.
struct Entry {
  std::filesystem::path path;
  os::FileStat stat;
  // The newer of the creation and last-write times: a file copied or extracted keeps its old
  // write time but gets a new creation time, so this is the safer age.
  WallTime touched;
};

// os::IFileSystem's listing, plus times. Never follows a reparse point and never reads content,
// so it cannot hydrate a cloud file.
class IFileTree {
 public:
  virtual ~IFileTree() = default;
  IFileTree(const IFileTree&) = delete;
  IFileTree& operator=(const IFileTree&) = delete;
  IFileTree(IFileTree&&) = delete;
  IFileTree& operator=(IFileTree&&) = delete;

  // `path` itself (a reparse point is described, not followed). Fails with not_found.
  [[nodiscard]] virtual Result<Entry> stat(const std::filesystem::path& path) const = 0;
  // The entries directly inside directory `path`. Fails with invalid_argument for a reparse
  // point, not_found when it is gone, access_denied when it cannot be read.
  [[nodiscard]] virtual Result<std::vector<Entry>> list(
      const std::filesystem::path& path) const = 0;

 protected:
  IFileTree() = default;
};

// The environment variables rule targets are expanded with (%TEMP%, %LOCALAPPDATA%, ...).
class IEnvironment {
 public:
  virtual ~IEnvironment() = default;
  IEnvironment(const IEnvironment&) = delete;
  IEnvironment& operator=(const IEnvironment&) = delete;
  IEnvironment(IEnvironment&&) = delete;
  IEnvironment& operator=(IEnvironment&&) = delete;

  // The value of variable `name` (case-insensitive), with 8.3 short names expanded when it is
  // an existing path; nullopt when it is not set.
  [[nodiscard]] virtual std::optional<std::wstring> variable(std::wstring_view name) const = 0;

 protected:
  IEnvironment() = default;
};

// Whether running processes use files or folders.
class IUsage {
 public:
  virtual ~IUsage() = default;
  IUsage(const IUsage&) = delete;
  IUsage& operator=(const IUsage&) = delete;
  IUsage(IUsage&&) = delete;
  IUsage& operator=(IUsage&&) = delete;

  // True when a process holds `file` open in a way that blocks deleting it, or when it cannot
  // be checked (the caller may not delete what it cannot open).
  [[nodiscard]] virtual bool locked(const std::filesystem::path& file) const = 0;
  // The current directory and the image folder of every running process that can be read.
  // Best effort: processes of other users or protected processes may be missing.
  [[nodiscard]] virtual std::vector<std::filesystem::path> process_folders() const = 0;

 protected:
  IUsage() = default;
};

// Read-only questions to git about one work tree. Implementations never write to a repository,
// never fetch and never start a background process (no fsmonitor, no optional locks).
class IGit {
 public:
  virtual ~IGit() = default;
  IGit(const IGit&) = delete;
  IGit& operator=(const IGit&) = delete;
  IGit(IGit&&) = delete;
  IGit& operator=(IGit&&) = delete;

  // The committer time of HEAD; nullopt when the repository has no commit yet.
  [[nodiscard]] virtual Result<std::optional<WallTime>> last_commit(
      const std::filesystem::path& work_tree) const = 0;
  // True when `git check-ignore` reports `entry` (inside `work_tree`) as ignored.
  [[nodiscard]] virtual Result<bool> ignored(const std::filesystem::path& work_tree,
                                             const std::filesystem::path& entry) const = 0;
  // True when the index tracks any file at or below `entry`.
  [[nodiscard]] virtual Result<bool> tracks_files(const std::filesystem::path& work_tree,
                                                  const std::filesystem::path& entry) const = 0;

 protected:
  IGit() = default;
};

}  // namespace myclean::clean
