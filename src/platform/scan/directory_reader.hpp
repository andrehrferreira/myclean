#pragma once

#include <filesystem>
#include <vector>

#include "core/model/result.hpp"
#include "core/space/scanner.hpp"

namespace myclean::platform {

// space::IDirectoryReader over NtQueryDirectoryFile (GetFileInformationByHandleEx with
// FileIdExtdDirectoryInfo and a 128 KB buffer): one call returns hundreds of entries with
// their file ids, times, sizes, attributes and reparse tags. The folder is opened with
// FILE_FLAG_OPEN_REPARSE_POINT, so a link is never followed. Sizes on disk come from the listing,
// except for a file the listing shows with less allocation than content although it is neither
// sparse, compressed nor a cloud file (Windows-compressed "WOF" files report zero there): that
// file's metadata is queried (never its content). Paths are passed as "\\?\" paths, so they may
// exceed MAX_PATH. Needs file ids (NTFS, ReFS): FAT volumes fail with io_error.
class Win32DirectoryReader final : public space::IDirectoryReader {
 public:
  Win32DirectoryReader() = default;

  [[nodiscard]] Result<std::vector<space::ListedEntry>> list(
      const std::filesystem::path& path) const override;
};

}  // namespace myclean::platform
