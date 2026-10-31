#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/model/result.hpp"

namespace myclean::rules {

// Read access to MyClean's own data folder. os::IFileSystem never reads file content (so that
// no scan can hydrate a cloud file); this interface is the one place that does, and only for
// data files. Tests use an in-memory implementation.
class IDataFiles {
 public:
  virtual ~IDataFiles() = default;
  IDataFiles(const IDataFiles&) = delete;
  IDataFiles& operator=(const IDataFiles&) = delete;
  IDataFiles(IDataFiles&&) = delete;
  IDataFiles& operator=(IDataFiles&&) = delete;

  // The regular files directly inside `dir`, in any order. Fails with not_found when `dir`
  // does not exist.
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> list(
      const std::filesystem::path& dir) const = 0;

  // The whole content of `file`. Fails with not_found when it does not exist.
  [[nodiscard]] virtual Result<std::string> read(const std::filesystem::path& file) const = 0;

 protected:
  IDataFiles() = default;
};

// IDataFiles over the real file system, with the portable C++ library.
class DiskDataFiles final : public IDataFiles {
 public:
  DiskDataFiles() = default;

  [[nodiscard]] Result<std::vector<std::filesystem::path>> list(
      const std::filesystem::path& dir) const override;
  [[nodiscard]] Result<std::string> read(const std::filesystem::path& file) const override;
};

}  // namespace myclean::rules
