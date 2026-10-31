#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"

// Old downloads (space spec, "Old Downloads"): files of the Downloads folder whose creation and
// last-write times are both older than the threshold. Last-access time is never used. Read-only.
namespace myclean::space {

inline constexpr std::chrono::days default_download_age{90};

// Where a file came from, as its Zone.Identifier stream records it ("Mark of the Web").
struct Origin {
  std::optional<int> zone_id;  // 3 = Internet, 2 = trusted sites, ...
  std::string host_url;        // the URL of the file itself; empty when not recorded
  std::string referrer_url;    // the page it was downloaded from; empty when not recorded
};

// Parses the text of a Zone.Identifier stream ("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=...").
[[nodiscard]] Origin parse_zone_identifier(std::string_view text);

// Reads the Zone.Identifier alternate data stream of a file. Implemented by src/platform/scan.
class IZoneIdentifierReader {
 public:
  virtual ~IZoneIdentifierReader() = default;
  IZoneIdentifierReader(const IZoneIdentifierReader&) = delete;
  IZoneIdentifierReader& operator=(const IZoneIdentifierReader&) = delete;
  IZoneIdentifierReader(IZoneIdentifierReader&&) = delete;
  IZoneIdentifierReader& operator=(IZoneIdentifierReader&&) = delete;

  // The stream's text, or nullopt when the file has no such stream.
  [[nodiscard]] virtual Result<std::optional<std::string>> read(
      const std::filesystem::path& file) const = 0;

 protected:
  IZoneIdentifierReader() = default;
};

struct OldDownload {
  std::filesystem::path path;
  Bytes size;
  Bytes size_on_disk;
  WallTime created;
  WallTime modified;
  std::int64_t age_days = 0;     // whole days since the newer of created and modified
  std::optional<Origin> origin;  // nullopt: unknown (no stream, unreadable, or a cloud file)
};

// The files of `downloads` (a scan of the Downloads folder) older than `min_age` at `now`,
// largest on disk first. Links and extra hard links are skipped; the origin of a cloud
// placeholder is not read, so that nothing is downloaded.
[[nodiscard]] std::vector<OldDownload> old_downloads(
    const ScanResult& downloads, WallTime now, const IZoneIdentifierReader& zones,
    std::chrono::days min_age = default_download_age);

}  // namespace myclean::space
