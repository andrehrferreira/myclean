#include "platform/scan/file_content.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <wil/resource.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/downloads.hpp"
#include "core/space/duplicates.hpp"
#include "core/space/scanner.hpp"
#include "platform/scan/directory_reader.hpp"
#include "sandbox.hpp"

namespace myclean::platform {
namespace {

namespace fs = std::filesystem;

std::string hex(const Result<space::Digest>& digest) {
  return digest ? space::to_hex(*digest) : "error: " + digest.error().message;
}

// Sets the creation and last-write times of `path` to `days_ago` days before now.
bool age_file(const fs::path& path, int days_ago) {
  const wil::unique_hfile file{::CreateFileW(extended(path).c_str(), FILE_WRITE_ATTRIBUTES, 0,
                                             nullptr, OPEN_EXISTING, 0, nullptr)};
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER ticks{.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
  ticks.QuadPart -= static_cast<ULONGLONG>(days_ago) * 24 * 3600 * 10'000'000;
  const FILETIME then{.dwLowDateTime = ticks.LowPart, .dwHighDateTime = ticks.HighPart};
  return file && ::SetFileTime(file.get(), &then, nullptr, &then) != FALSE;
}

TEST(Win32ContentHasher, HashesWithSha256) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "abc.txt", 0);
  write_file(sandbox.root() / "abc.txt", 1, 'a');
  const Win32ContentHasher hasher;

  // SHA-256("a")
  EXPECT_EQ(hex(hasher.hash(sandbox.root() / "abc.txt", std::nullopt)),
            "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
}

TEST(Win32ContentHasher, HashesTheHeadOrTheWholeContent) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "one.bin", 200'000, 'h');
  write_file(sandbox.root() / "two.bin", 200'000, 'h');
  write_file(sandbox.root() / "three.bin", 200'000, 'h');
  {
    std::ofstream tail(extended(sandbox.root() / "three.bin"), std::ios::binary | std::ios::in);
    tail.seekp(150'000);
    tail.put('t');
  }
  const Win32ContentHasher hasher;
  const std::optional<Bytes> head = space::head_hash_bytes;

  EXPECT_EQ(hex(hasher.hash(sandbox.root() / "one.bin", std::nullopt)),
            hex(hasher.hash(sandbox.root() / "two.bin", std::nullopt)));
  EXPECT_EQ(hex(hasher.hash(sandbox.root() / "one.bin", head)),
            hex(hasher.hash(sandbox.root() / "three.bin", head)));
  EXPECT_NE(hex(hasher.hash(sandbox.root() / "one.bin", std::nullopt)),
            hex(hasher.hash(sandbox.root() / "three.bin", std::nullopt)));
}

TEST(Win32ContentHasher, ReadsAWindowsCompressedFileThroughItsFilter) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "plain.bin", 300'000, 'w');
  write_file(sandbox.root() / "wof.bin", 300'000, 'w');
  if (!compress_wof(sandbox.root() / "wof.bin")) {
    GTEST_SKIP() << "WOF compression is not available on this volume";
  }
  const Win32ContentHasher hasher;

  EXPECT_EQ(hex(hasher.hash(sandbox.root() / "wof.bin", std::nullopt)),
            hex(hasher.hash(sandbox.root() / "plain.bin", std::nullopt)));
}

TEST(Win32ContentHasher, RefusesALinkAFolderAndACloudFile) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "target" / "a.bin", 10);
  ASSERT_TRUE(make_junction(sandbox.root() / "link", sandbox.root() / "target"));
  write_file(sandbox.root() / "cloud.bin", 10);
  ASSERT_TRUE(mark_offline(sandbox.root() / "cloud.bin"));
  const Win32ContentHasher hasher;

  EXPECT_EQ(hasher.hash(sandbox.root() / "link", std::nullopt).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(hasher.hash(sandbox.root() / "target", std::nullopt).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(hasher.hash(sandbox.root() / "cloud.bin", std::nullopt).error().code, Errc::refused);
}

TEST(Win32ContentReader, ReadsARangeShortAtTheEndAndNothingPastIt) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "a.zip", 3'000'000, 'a');
  {
    std::ofstream file(extended(sandbox.root() / "a.zip"), std::ios::binary | std::ios::in);
    file.seekp(2'999'990);
    file.write("0123456789", 10);
  }
  const Win32ContentReader reader;

  EXPECT_EQ(reader.read(sandbox.root() / "a.zip", 2'999'990, 4).value(), "0123");
  EXPECT_EQ(reader.read(sandbox.root() / "a.zip", 2'999'995, 100).value(), "56789");
  EXPECT_EQ(reader.read(sandbox.root() / "a.zip", 3'000'000, 10).value(), "");
  EXPECT_EQ(reader.read(sandbox.root() / "a.zip", 5'000'000, 10).value(), "");
  // Longer than one read chunk.
  const Result<std::string> long_read = reader.read(sandbox.root() / "a.zip", 0, 2'500'000);
  ASSERT_TRUE(long_read.has_value()) << long_read.error().message;
  EXPECT_EQ(long_read->size(), 2'500'000U);
  EXPECT_EQ(long_read->find_first_not_of('a'), std::string::npos);
}

TEST(Win32ContentReader, RefusesALinkAFolderAndACloudFile) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "target" / "a.rar", 10);
  ASSERT_TRUE(make_junction(sandbox.root() / "link", sandbox.root() / "target"));
  write_file(sandbox.root() / "cloud.rar", 10);
  ASSERT_TRUE(mark_offline(sandbox.root() / "cloud.rar"));
  const Win32ContentReader reader;

  EXPECT_EQ(reader.read(sandbox.root() / "link", 0, 8).error().code, Errc::invalid_argument);
  EXPECT_EQ(reader.read(sandbox.root() / "target", 0, 8).error().code, Errc::invalid_argument);
  EXPECT_EQ(reader.read(sandbox.root() / "cloud.rar", 0, 8).error().code, Errc::refused);
}

TEST(Win32ZoneIdentifierReader, ReadsTheStreamInUtf8OrUtf16) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "setup.zip", 10);
  write_stream(sandbox.root() / "setup.zip", L"Zone.Identifier",
               "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://h/setup.zip\r\n");
  write_file(sandbox.root() / "wide.zip", 10);
  write_stream(sandbox.root() / "wide.zip", L"Zone.Identifier",
               std::string_view{"\xFF\xFE[\0Z\0]\0", 8});
  write_file(sandbox.root() / "plain.zip", 10);
  const Win32ZoneIdentifierReader reader;

  const auto zone = reader.read(sandbox.root() / "setup.zip");
  ASSERT_TRUE(zone.has_value());
  ASSERT_TRUE(zone->has_value());
  EXPECT_EQ(space::parse_zone_identifier(**zone).host_url, "https://h/setup.zip");
  EXPECT_EQ(reader.read(sandbox.root() / "wide.zip").value(), std::optional<std::string>{"[Z]"});
  EXPECT_EQ(reader.read(sandbox.root() / "plain.zip").value(), std::nullopt);
}

TEST(Win32ZoneIdentifierReader, RefusesACloudFile) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "cloud.zip", 10);
  ASSERT_TRUE(mark_offline(sandbox.root() / "cloud.zip"));
  const Win32ZoneIdentifierReader reader;

  EXPECT_EQ(reader.read(sandbox.root() / "cloud.zip").error().code, Errc::refused);
}

TEST(SpaceAnalyses, FindOldDownloadsAndDuplicatesOnDisk) {
  const Sandbox sandbox;
  write_file(sandbox.root() / "old.zip", 5000);
  write_stream(sandbox.root() / "old.zip", L"Zone.Identifier",
               "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://h/old.zip\r\n");
  ASSERT_TRUE(age_file(sandbox.root() / "old.zip", 120));
  write_file(sandbox.root() / "new.zip", 5000, 'n');
  write_file(sandbox.root() / "photos" / "a.jpg", 70'000, 'p');
  write_file(sandbox.root() / "photos" / "copy.jpg", 70'000, 'p');
  ASSERT_TRUE(::CreateHardLinkW(extended(sandbox.root() / "photos" / "hard.jpg").c_str(),
                                extended(sandbox.root() / "photos" / "a.jpg").c_str(),
                                nullptr) != FALSE);
  const Win32DirectoryReader directories;
  const Result<space::ScanResult> scan = space::scan(directories, sandbox.root());
  ASSERT_TRUE(scan.has_value());

  const std::vector<space::OldDownload> old =
      space::old_downloads(*scan, std::chrono::system_clock::now(), Win32ZoneIdentifierReader{});
  const space::DuplicateReport duplicates =
      space::find_duplicates(std::span{&*scan, 1}, Win32ContentHasher{});

  ASSERT_EQ(old.size(), 1U);
  EXPECT_EQ(old[0].path, sandbox.root() / "old.zip");
  EXPECT_GE(old[0].age_days, 119);
  ASSERT_TRUE(old[0].origin.has_value());
  EXPECT_EQ(old[0].origin.value_or(space::Origin{}).host_url, "https://h/old.zip");
  ASSERT_EQ(duplicates.groups.size(), 1U);  // the hard link is the same file, not a copy
  ASSERT_EQ(duplicates.groups[0].files.size(), 2U);
  EXPECT_EQ(duplicates.groups[0].files[0].path, sandbox.root() / "photos" / "a.jpg");
  EXPECT_EQ(duplicates.groups[0].files[1].path, sandbox.root() / "photos" / "copy.jpg");
  EXPECT_TRUE(duplicates.errors.empty());
}

TEST(DownloadsFolder, IsAnAbsolutePath) {
  const Result<fs::path> folder = downloads_folder();

  ASSERT_TRUE(folder.has_value()) << folder.error().message;
  EXPECT_TRUE(folder->is_absolute());
}

}  // namespace
}  // namespace myclean::platform
