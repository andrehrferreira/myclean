#include "core/space/duplicates.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::scan_ok;
namespace fs = std::filesystem;

// Hashes in-memory content: the digest holds the content's std::hash in its first bytes.
class FakeHasher final : public IContentHasher {
 public:
  std::map<fs::path, std::string> contents;
  mutable std::vector<std::pair<fs::path, std::optional<Bytes>>> calls;
  std::function<void()> on_hash;

  [[nodiscard]] Result<Digest> hash(const fs::path& file,
                                    std::optional<Bytes> limit) const override {
    calls.emplace_back(file, limit);
    if (on_hash) {
      on_hash();
    }
    const auto content = contents.find(file);
    if (content == contents.end()) {
      return fail(Errc::in_use, "locked");
    }
    const std::string part =
        limit ? content->second.substr(0, static_cast<std::size_t>(limit->count()))
              : content->second;
    Digest digest{};
    const std::size_t value = std::hash<std::string>{}(part);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      digest.at(i) = static_cast<std::uint8_t>(value >> (8 * i));
    }
    return digest;
  }

  [[nodiscard]] std::size_t calls_for(const fs::path& file) const {
    return static_cast<std::size_t>(
        std::ranges::count(calls, file, &std::pair<fs::path, std::optional<Bytes>>::first));
  }
};

// A scan of one folder whose files have the given contents; ids are unique unless given.
struct Drive {
  FakeDirectoryReader reader;
  fs::path root;
  std::uint64_t volume;

  void add(FakeHasher& hasher, const std::string& name, const std::string& content,
           std::uint8_t id) {
    ListedEntry entry = testing::file(name, content.size(), id);
    entry.identity.volume_serial = volume;
    reader.listings[root].push_back(entry);
    hasher.contents[root / name] = content;
  }
};

TEST(Duplicates, GroupsIdenticalFilesAcrossDrives) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\photos", .volume = 4};
  Drive k{.root = L"K:\\archive", .volume = 11};
  d.add(hasher, "a.jpg", "same picture", 1);
  d.add(hasher, "b.jpg", "other image!", 2);       // same size, different content
  k.add(hasher, "a-copy.jpg", "same picture", 1);  // same id on another volume: another file

  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root), scan_ok(k.reader, k.root)};
  const DuplicateReport report = find_duplicates(scans, hasher);

  ASSERT_EQ(report.groups.size(), 1U);
  const DuplicateGroup& group = report.groups[0];
  EXPECT_EQ(group.size, Bytes{12});
  EXPECT_EQ(group.reclaimable, Bytes{12});
  ASSERT_EQ(group.files.size(), 2U);
  EXPECT_EQ(group.files[0].path, fs::path{L"D:\\photos\\a.jpg"});
  EXPECT_EQ(group.files[1].path, fs::path{L"K:\\archive\\a-copy.jpg"});
  EXPECT_FALSE(report.cancelled);
  // Small files are hashed once, whole.
  EXPECT_EQ(hasher.calls_for(L"D:\\photos\\a.jpg"), 1U);
  EXPECT_FALSE(hasher.calls.front().second.has_value());
}

TEST(Duplicates, SeparatesFilesWhoseFirst64KbMatchButNotTheRest) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\data", .volume = 1};
  const std::string head(70'000, 'h');
  d.add(hasher, "one.bin", head + "1", 1);
  d.add(hasher, "two.bin", head + "2", 2);
  d.add(hasher, "three.bin", head + "1", 3);

  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root)};
  const DuplicateReport report = find_duplicates(scans, hasher);

  ASSERT_EQ(report.groups.size(), 1U);
  ASSERT_EQ(report.groups[0].files.size(), 2U);
  EXPECT_EQ(report.groups[0].files[0].path, d.root / "one.bin");
  EXPECT_EQ(report.groups[0].files[1].path, d.root / "three.bin");
  EXPECT_EQ(hasher.calls_for(d.root / "two.bin"), 2U);  // head, then whole
  EXPECT_EQ(hasher.calls.front().second, head_hash_bytes);
}

TEST(Duplicates, NeverReadsAFileOfAUniqueSize) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\data", .volume = 1};
  d.add(hasher, "unique.bin", "abcdef", 1);
  d.add(hasher, "x.bin", "xyz", 2);
  d.add(hasher, "y.bin", "xyz", 3);

  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root)};
  const DuplicateReport report = find_duplicates(scans, hasher);

  EXPECT_EQ(report.groups.size(), 1U);
  EXPECT_EQ(hasher.calls_for(d.root / "unique.bin"), 0U);
}

TEST(Duplicates, TreatsHardLinksAsOneFileAndSkipsLinksCloudAndEmptyFiles) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\data", .volume = 1};
  d.add(hasher, "a.bin", "content", 1);
  d.add(hasher, "hardlink.bin", "content", 1);
  d.add(hasher, "empty1", "", 2);
  d.add(hasher, "empty2", "", 3);
  ListedEntry cloud = testing::file("cloud.bin", 7, 4);
  cloud.attributes.offline = true;
  ListedEntry symlink = testing::file("sym.bin", 7, 5);
  symlink.attributes.reparse_point = true;
  symlink.reparse_tag = testing::tag_symlink;
  d.reader.listings[d.root].push_back(cloud);
  d.reader.listings[d.root].push_back(symlink);

  // The same folder scanned twice: overlapping scans must not report a file against itself.
  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root), scan_ok(d.reader, d.root)};
  const DuplicateReport report = find_duplicates(scans, hasher);

  EXPECT_TRUE(report.groups.empty());
  EXPECT_TRUE(hasher.calls.empty());
}

TEST(Duplicates, FiltersByExtensionAndMinimumSizeAndRecordsErrors) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\data", .volume = 1};
  d.add(hasher, "a.JPG", "pixels", 1);
  d.add(hasher, "b.jpg", "pixels", 2);
  d.add(hasher, "c.txt", "pixels", 3);
  d.add(hasher, "tiny1.jpg", "p", 4);
  d.add(hasher, "tiny2.jpg", "p", 5);
  d.add(hasher, "locked.jpg", "pixels", 6);
  hasher.contents.erase(d.root / "locked.jpg");

  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root)};
  const DuplicateReport report = find_duplicates(
      scans, hasher, DuplicateOptions{.min_size = Bytes{2}, .extensions = {".jpg", ".png"}});

  ASSERT_EQ(report.groups.size(), 1U);
  ASSERT_EQ(report.groups[0].files.size(), 2U);
  EXPECT_EQ(report.groups[0].files[0].path, d.root / "a.JPG");
  EXPECT_EQ(hasher.calls_for(d.root / "c.txt"), 0U);
  EXPECT_EQ(hasher.calls_for(d.root / "tiny1.jpg"), 0U);
  ASSERT_EQ(report.errors.size(), 1U);
  EXPECT_EQ(report.errors[0].path, d.root / "locked.jpg");
  EXPECT_EQ(report.errors[0].error.code, Errc::in_use);
}

TEST(Duplicates, StopsWhenCancelled) {
  FakeHasher hasher;
  Drive d{.root = L"D:\\data", .volume = 1};
  d.add(hasher, "a", "1234", 1);
  d.add(hasher, "b", "1234", 2);
  d.add(hasher, "c", "12", 3);
  d.add(hasher, "e", "12", 4);
  std::stop_source source;
  hasher.on_hash = [&source] { source.request_stop(); };

  const std::vector<ScanResult> scans = {scan_ok(d.reader, d.root)};
  const DuplicateReport report = find_duplicates(scans, hasher, {}, source.get_token());

  EXPECT_TRUE(report.cancelled);
  EXPECT_TRUE(report.groups.empty());
  EXPECT_EQ(hasher.calls.size(), 1U);
}

TEST(Duplicates, WritesDigestsAsHex) {
  Digest digest{};
  digest[0] = 0xAB;
  digest[31] = 0x01;
  const std::string hex = to_hex(digest);
  EXPECT_EQ(hex.size(), 64U);
  EXPECT_EQ(hex.substr(0, 2), "ab");
  EXPECT_EQ(hex.substr(62), "01");
}

}  // namespace
}  // namespace myclean::space
