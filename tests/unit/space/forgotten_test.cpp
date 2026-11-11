#include "core/space/forgotten.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using std::chrono::days;
using testing::FakeDirectoryReader;
using testing::scan_ok;
using namespace myclean::literals;
namespace fs = std::filesystem;

const fs::path videos{L"C:\\Users\\owner\\Videos"};
const fs::path documents{L"C:\\Users\\owner\\Documents"};
const WallTime now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 11};

ListedEntry aged(const std::string& name, Bytes size, days created_ago, days modified_ago) {
  static std::uint8_t next_id = 0;
  ListedEntry entry = testing::file(name, size.count(), ++next_id);
  entry.created = now - created_ago;
  entry.modified = now - modified_ago;
  return entry;
}

TEST(ForgottenFiles, ListsLargeIdleFilesOfEveryScanLargestFirst) {
  FakeDirectoryReader reader;
  reader.listings[videos] = {aged("holiday-2019.mkv", 3_GiB, days{900}, days{900}),
                             testing::folder("raw")};
  reader.listings[videos / "raw"] = {aged("take-1.mov", 1_GiB, days{400}, days{300})};
  reader.listings[documents] = {aged("backup.iso", 5_GiB, days{200}, days{190})};
  const std::vector<ScanResult> scans = {scan_ok(reader, videos), scan_ok(reader, documents)};

  const std::vector<ForgottenFile> forgotten = forgotten_files(scans, now);

  ASSERT_EQ(forgotten.size(), 3U);
  EXPECT_EQ(forgotten[0].path, documents / "backup.iso");
  EXPECT_EQ(forgotten[0].size_on_disk, 5_GiB);
  EXPECT_EQ(forgotten[0].idle_days, 190);  // the newer of created and modified
  EXPECT_EQ(forgotten[1].path, videos / "holiday-2019.mkv");
  EXPECT_EQ(forgotten[2].path, videos / "raw" / "take-1.mov");
  EXPECT_EQ(forgotten[2].idle_days, 300);
}

TEST(ForgottenFiles, SkipsSmallRecentLinkedAndCloudFiles) {
  FakeDirectoryReader reader;
  ListedEntry cloud = aged("cloud.mkv", 2_GiB, days{400}, days{400});
  cloud.attributes.recall_on_data_access = true;
  ListedEntry link = aged("link.mkv", 2_GiB, days{400}, days{400});
  link.attributes.reparse_point = true;
  link.reparse_tag = testing::tag_symlink;
  reader.listings[videos] = {
      cloud,
      link,
      aged("small.mp4", 100_MiB, days{400}, days{400}),
      aged("copied-in.mkv", 2_GiB, days{20}, days{500}),  // created recently: a fresh copy
      aged("edited.mkv", 2_GiB, days{500}, days{20}),
      aged("boundary.mkv", 2_GiB, days{180}, days{180}),
  };

  EXPECT_TRUE(forgotten_files(std::vector{scan_ok(reader, videos)}, now).empty());
}

TEST(ForgottenFiles, HonoursTheSizeAndIdleThresholds) {
  FakeDirectoryReader reader;
  reader.listings[documents] = {aged("thesis-draft.docx", 20_MiB, days{40}, days{40})};
  const std::vector<ScanResult> scans = {scan_ok(reader, documents)};

  EXPECT_TRUE(forgotten_files(scans, now).empty());
  const std::vector<ForgottenFile> forgotten = forgotten_files(scans, now, 10_MiB, days{30});
  ASSERT_EQ(forgotten.size(), 1U);
  EXPECT_EQ(forgotten[0].path, documents / "thesis-draft.docx");
}

}  // namespace
}  // namespace myclean::space
