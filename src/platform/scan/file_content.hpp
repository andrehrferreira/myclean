#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"

// The readers of file content behind the space analyses. Both check the entry before opening
// it and refuse a link or a cloud file (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN, OFFLINE), so
// nothing is followed or downloaded; files are opened for reading only and never written.
namespace myclean::platform {

// Reads the "<file>:Zone.Identifier" stream. UTF-16 text (with its BOM) is returned as UTF-8.
class Win32ZoneIdentifierReader final : public space::IZoneIdentifierReader {
 public:
  Win32ZoneIdentifierReader() = default;

  [[nodiscard]] Result<std::optional<std::string>> read(
      const std::filesystem::path& file) const override;
};

// SHA-256 through BCrypt (the spec names BLAKE3, which is not among the project's libraries).
// Reads sequentially at low I/O priority. A Windows-compressed (WOF) or deduplicated file is
// read through its file system filter, so the hash is of the content a program sees.
class Win32ContentHasher final : public space::IContentHasher {
 public:
  Win32ContentHasher() = default;

  [[nodiscard]] Result<space::Digest> hash(const std::filesystem::path& file,
                                           std::optional<Bytes> limit) const override;
};

// The current user's Downloads folder (FOLDERID_Downloads).
[[nodiscard]] Result<std::filesystem::path> downloads_folder();

}  // namespace myclean::platform
