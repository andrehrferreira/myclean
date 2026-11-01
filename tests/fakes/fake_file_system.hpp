#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/model/file_identity.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"

namespace myclean::fakes {

// An in-memory IFileSystem with Windows semantics: case-insensitive paths, one volume per root
// name, reparse points that are never followed. Tests build the tree with add_* and simulate
// outside changes with erase(); every mutating IFileSystem call is recorded in operations().
class FakeFileSystem final : public os::IFileSystem {
 public:
  enum class OperationKind { remove, rename, move_file, copy_file, flush, create_directories };

  struct Operation {
    OperationKind kind;
    std::filesystem::path from;  // flush: the first file
    std::filesystem::path to;    // empty for remove, flush and create_directories

    friend bool operator==(const Operation&, const Operation&) = default;
  };

  FakeFileSystem() = default;

  // Test setup. Missing parent directories are created. Throws std::logic_error when the path
  // exists, is not absolute, or would lie inside a file or a reparse point.
  FileIdentity add_directory(const std::filesystem::path& path, os::FileAttributes attributes = {});
  // A cloud placeholder (see FileAttributes::cloud_placeholder) gets a size on disk of zero.
  FileIdentity add_file(const std::filesystem::path& path, Bytes size,
                        os::FileAttributes attributes = {});
  // Gives the last component of `path` an 8.3 alias such as "PROGRA~1".
  void set_short_name(const std::filesystem::path& path, const std::wstring& short_name);
  // Makes every later remove, rename, move_file, copy_file, flush or create_directories that
  // names `path` fail; with `only`, just the operations of that kind.
  void fail_on(const std::filesystem::path& path, Errc code,
               std::optional<OperationKind> only = std::nullopt);
  // Removes `path` and everything below it without recording an operation: a change made by
  // someone else. Throws std::logic_error when `path` does not exist.
  void erase(const std::filesystem::path& path);
  // Runs `hook` at the start of every later remove, before the entry is looked up: a change
  // someone else makes between a caller's last check and its delete.
  void on_remove(std::function<void(const std::filesystem::path&)> hook) {
    on_remove_ = std::move(hook);
  }

  [[nodiscard]] bool exists(const std::filesystem::path& path) const;
  [[nodiscard]] const std::vector<Operation>& operations() const { return operations_; }

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

 private:
  struct Node {
    std::filesystem::path path;  // as spelled when created
    os::FileStat stat;
    std::wstring short_name;
  };
  using Tree = std::map<std::wstring, Node>;

  [[nodiscard]] static std::wstring key_of(const std::filesystem::path& path);
  [[nodiscard]] static std::wstring child_prefix(const std::wstring& key);
  [[nodiscard]] static std::uint64_t volume_serial_of(const std::filesystem::path& path);
  [[nodiscard]] std::optional<Errc> injected_failure(const std::filesystem::path& path,
                                                     OperationKind kind) const;
  [[nodiscard]] bool has_children(const std::wstring& key) const;
  [[nodiscard]] std::vector<const Node*> children_of(const std::wstring& key) const;
  // Fails unless the parent of `path` exists as a directory that is not a reparse point.
  [[nodiscard]] Result<void> check_parent(const std::filesystem::path& path) const;
  FileIdentity insert(const std::filesystem::path& path, os::FileStat stat);
  void ensure_parents(const std::filesystem::path& path);
  FileIdentity next_identity(const std::filesystem::path& path);
  // remove() with an optional identity check.
  [[nodiscard]] Result<void> remove_entry(const std::filesystem::path& path,
                                          const FileIdentity* expected);

  Tree tree_;
  struct Failure {
    Errc code = Errc::io_error;
    std::optional<OperationKind> only;
  };
  std::map<std::wstring, Failure> failures_;
  std::vector<Operation> operations_;
  std::function<void(const std::filesystem::path&)> on_remove_;
  std::uint64_t next_file_id_ = 1;
};

}  // namespace myclean::fakes
