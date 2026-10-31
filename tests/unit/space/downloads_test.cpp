#include "core/space/downloads.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using std::chrono::days;
using testing::FakeDirectoryReader;
using testing::file;
using testing::scan_ok;
namespace fs = std::filesystem;

const fs::path downloads{L"C:\\Users\\owner\\Downloads"};
const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 10 / 31};

class FakeZones final : public IZoneIdentifierReader {
 public:
  std::map<fs::path, std::string> streams;
  mutable std::set<fs::path> read_paths;

  [[nodiscard]] Result<std::optional<std::string>> read(const fs::path& path) const override {
    read_paths.insert(path);
    const auto stream = streams.find(path);
    if (stream == streams.end()) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>{stream->second};
  }
};

ListedEntry aged(const std::string& name, std::uint64_t size, days created_ago, days modified_ago) {
  static std::uint8_t next_id = 0;
  ListedEntry entry = file(name, size, ++next_id);
  entry.created = now - created_ago;
  entry.modified = now - modified_ago;
  return entry;
}

TEST(ZoneIdentifier, ParsesZoneAndUrls) {
  const Origin origin = parse_zone_identifier(
      "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.com/page\r\n"
      "HostUrl=https://cdn.example.com/setup.zip\r\n");

  EXPECT_EQ(origin.zone_id, 3);
  EXPECT_EQ(origin.host_url, "https://cdn.example.com/setup.zip");
  EXPECT_EQ(origin.referrer_url, "https://example.com/page");
}

TEST(ZoneIdentifier, IgnoresCaseUnknownKeysAndBadZones) {
  const Origin origin = parse_zone_identifier("[zonetransfer]\nzoneid=abc\nhosturl = x \nOther=1");

  EXPECT_FALSE(origin.zone_id.has_value());
  EXPECT_EQ(origin.host_url, "x");
  EXPECT_TRUE(origin.referrer_url.empty());
}

TEST(OldDownloads, ListsAnOldFileWithItsSizeAgeAndOrigin) {
  FakeDirectoryReader reader;
  reader.listings[downloads] = {aged("setup.zip", 4096, days{120}, days{120})};
  FakeZones zones;
  zones.streams[downloads / "setup.zip"] = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://h/s.zip";

  const std::vector<OldDownload> old = old_downloads(scan_ok(reader, downloads), now, zones);

  ASSERT_EQ(old.size(), 1U);
  EXPECT_EQ(old[0].path, downloads / "setup.zip");
  EXPECT_EQ(old[0].size_on_disk, Bytes{4096});
  EXPECT_EQ(old[0].age_days, 120);
  ASSERT_TRUE(old[0].origin.has_value());
  EXPECT_EQ(old[0].origin.value_or(Origin{}).host_url, "https://h/s.zip");
}

TEST(OldDownloads, SkipsAFileModifiedRecentlyOrCreatedRecently) {
  FakeDirectoryReader reader;
  reader.listings[downloads] = {aged("edited.txt", 10, days{200}, days{10}),
                                aged("copied.txt", 10, days{5}, days{300}),
                                aged("boundary.txt", 10, days{90}, days{90})};
  const FakeZones zones;

  EXPECT_TRUE(old_downloads(scan_ok(reader, downloads), now, zones).empty());
}

TEST(OldDownloads, ShowsAnUnknownOriginWithoutAStream) {
  FakeDirectoryReader reader;
  reader.listings[downloads] = {aged("a.pdf", 10, days{100}, days{100})};
  const FakeZones zones;

  const std::vector<OldDownload> old = old_downloads(scan_ok(reader, downloads), now, zones);

  ASSERT_EQ(old.size(), 1U);
  EXPECT_FALSE(old[0].origin.has_value());
}

TEST(OldDownloads, NeverReadsTheStreamOfACloudFileAndHonoursTheThreshold) {
  FakeDirectoryReader reader;
  ListedEntry cloud = aged("cloud.mkv", 0, days{100}, days{100});
  cloud.attributes.recall_on_data_access = true;
  reader.listings[downloads] = {cloud, aged("old.bin", 50, days{40}, days{40}),
                                aged("older.bin", 60, days{45}, days{45}), testing::folder("sub")};
  reader.listings[downloads / "sub"] = {aged("nested.bin", 70, days{45}, days{50})};
  const FakeZones zones;

  const std::vector<OldDownload> old =
      old_downloads(scan_ok(reader, downloads), now, zones, days{30});

  ASSERT_EQ(old.size(), 4U);
  EXPECT_EQ(old[0].path, downloads / "sub" / "nested.bin");  // largest first
  EXPECT_EQ(old[3].path, downloads / "cloud.mkv");
  EXPECT_FALSE(zones.read_paths.contains(downloads / "cloud.mkv"));
  EXPECT_TRUE(zones.read_paths.contains(downloads / "old.bin"));
}

}  // namespace
}  // namespace myclean::space
