#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/model/result.hpp"

// Win32 implementations of the cleaning engine's read-only probes (core/clean/probes.hpp).
namespace myclean::platform {

// IFileTree over Win32: one handle per folder, listed with FileIdExtdDirectoryInfo. Every entry
// is opened with FILE_FLAG_OPEN_REPARSE_POINT, so a link is described and never followed, and
// no call reads file content, so none hydrates a cloud file. Needs file ids (NTFS, ReFS).
class Win32FileTree final : public clean::IFileTree {
 public:
  Win32FileTree() = default;

  [[nodiscard]] Result<clean::Entry> stat(const std::filesystem::path& path) const override;
  [[nodiscard]] Result<std::vector<clean::Entry>> list(
      const std::filesystem::path& path) const override;
};

// The calling process's environment block.
class Win32Environment final : public clean::IEnvironment {
 public:
  Win32Environment() = default;

  [[nodiscard]] std::optional<std::wstring> variable(std::wstring_view name) const override;
};

// Lock checks and process folders.
//   locked():          opens the file for DELETE with every sharing mode allowed and closes it
//                      at once. It fails with a sharing violation exactly when another handle
//                      does not share delete, which is when deleting the file would fail too.
//                      No data is read or written; the handle exists for microseconds.
//   process_folders(): each process's image folder (QueryFullProcessImageNameW) and, for
//                      64-bit processes whose memory can be read, its current directory (read
//                      from its process parameters). Without elevation, only the caller's own
//                      processes can be read; 32-bit processes contribute their image only.
class Win32Usage final : public clean::IUsage {
 public:
  Win32Usage() = default;

  [[nodiscard]] bool locked(const std::filesystem::path& file) const override;
  [[nodiscard]] std::vector<std::filesystem::path> process_folders() const override;
};

}  // namespace myclean::platform
