#include "core/space/scanner.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::file;
using testing::folder;
using testing::folder_at;
using testing::junction;
using testing::scan_ok;
namespace fs = std::filesystem;

const fs::path root{L"C:\\data"};

TEST(Scanner, AddsUpSizesAndCountsFromTheLeaves) {
  FakeDirectoryReader reader;
  reader.listings[root] = {file("a.bin", 100, 1), folder("sub")};
  reader.listings[root / "sub"] = {file("b.bin", 200, 2), file("c.bin", 300, 3), folder("deep")};
  reader.listings[root / "sub" / "deep"] = {file("d.bin", 400, 4)};

  const ScanResult result = scan_ok(reader, root);

  EXPECT_FALSE(result.cancelled);
  ASSERT_EQ(result.folders.size(), 3U);
  EXPECT_EQ(result.root().path, root);
  EXPECT_EQ(result.root().size_on_disk, Bytes{1000});
  EXPECT_EQ(result.root().own_size_on_disk, Bytes{100});
  EXPECT_EQ(result.root().file_count, 4U);
  EXPECT_EQ(result.root().folder_count, 2U);
  const Folder& sub = folder_at(result, root / "sub");
  EXPECT_EQ(sub.size_on_disk, Bytes{900});
  EXPECT_EQ(sub.file_count, 3U);
  EXPECT_EQ(sub.folder_count, 1U);
  EXPECT_EQ(result.folders[sub.parent].path, root);
  EXPECT_EQ(path_of(sub, sub.files.front()), root / "sub" / "b.bin");
}

TEST(Scanner, ListsAJunctionAsALinkWithoutEnteringIt) {
  FakeDirectoryReader reader;
  reader.listings[root] = {file("package.json", 10, 1), junction("node_modules")};
  reader.listings[root / "node_modules"] = {file("huge.bin", 1'000'000, 2)};

  const ScanResult result = scan_ok(reader, root);

  EXPECT_FALSE(reader.was_listed(root / "node_modules"));
  ASSERT_EQ(result.folders.size(), 1U);
  ASSERT_EQ(result.root().files.size(), 2U);
  const FileItem& link = result.root().files[0];  // sorted by name
  EXPECT_EQ(link.name, fs::path{"node_modules"});
  EXPECT_TRUE(link.link);
  EXPECT_EQ(link.reparse_tag, testing::tag_mount_point);
  EXPECT_EQ(link.size_on_disk, Bytes{});
  EXPECT_EQ(result.root().size_on_disk, Bytes{10});
  EXPECT_EQ(result.root().file_count, 1U);
  EXPECT_EQ(result.link_count, 1U);
}

TEST(Scanner, TreatsNameSurrogatesAsLinksAndOtherReparseFilesAsFiles) {
  FakeDirectoryReader reader;
  ListedEntry symlink = file("link.txt", 50, 1);
  symlink.attributes.reparse_point = true;
  symlink.reparse_tag = testing::tag_symlink;
  ListedEntry cloud = file("synced.docx", 70, 2);
  cloud.attributes.reparse_point = true;
  cloud.reparse_tag = testing::tag_cloud;
  reader.listings[root] = {symlink, cloud};

  const ScanResult result = scan_ok(reader, root);

  ASSERT_EQ(result.root().files.size(), 2U);
  EXPECT_TRUE(result.root().files[0].link);
  EXPECT_FALSE(result.root().files[1].link);
  EXPECT_EQ(result.root().size_on_disk, Bytes{70});
}

TEST(Scanner, KeepsTheLocalSizeOfACloudPlaceholder) {
  FakeDirectoryReader reader;
  ListedEntry placeholder = file("video.mp4", 0, 1);
  placeholder.size = Bytes{5'000'000'000};
  placeholder.attributes.recall_on_data_access = true;
  reader.listings[root] = {placeholder};

  const ScanResult result = scan_ok(reader, root);

  const FileItem& item = result.root().files.front();
  EXPECT_TRUE(item.placeholder);
  EXPECT_FALSE(item.link);
  EXPECT_EQ(item.size_on_disk, Bytes{});
  EXPECT_EQ(item.size, Bytes{5'000'000'000});
  EXPECT_EQ(result.root().size_on_disk, Bytes{});
  EXPECT_EQ(result.placeholder_count, 1U);
}

TEST(Scanner, RecordsAnUnreadableFolderAndGoesOn) {
  FakeDirectoryReader reader;
  reader.listings[root] = {folder("locked"), folder("open")};
  reader.errors.insert_or_assign(root / "locked",
                                 Error{.code = Errc::access_denied, .message = "denied"});
  reader.listings[root / "open"] = {file("x.bin", 30, 1)};

  const ScanResult result = scan_ok(reader, root);

  EXPECT_EQ(result.error_count, 1U);
  const Folder& locked = folder_at(result, root / "locked");
  ASSERT_TRUE(locked.error.has_value());
  EXPECT_EQ(locked.error.value_or(Error{}).code, Errc::access_denied);
  EXPECT_EQ(result.root().size_on_disk, Bytes{30});
  EXPECT_EQ(result.root().folder_count, 2U);
}

TEST(Scanner, FailsWhenTheRootCannotBeListed) {
  FakeDirectoryReader reader;
  reader.errors.insert_or_assign(root, Error{.code = Errc::access_denied, .message = "denied"});

  const Result<ScanResult> result = scan(reader, root);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::access_denied);
}

TEST(Scanner, RejectsARelativeRootAndABadWorkerCount) {
  const FakeDirectoryReader reader;
  EXPECT_EQ(scan(reader, fs::path{"data"}).error().code, Errc::invalid_argument);
  EXPECT_EQ(scan(reader, root, ScanOptions{.workers = 0}).error().code, Errc::invalid_argument);
  EXPECT_EQ(scan(reader, root, ScanOptions{.workers = 65}).error().code, Errc::invalid_argument);
}

TEST(Scanner, CountsHardLinksOnceInTheFirstFolderByName) {
  FakeDirectoryReader reader;
  reader.listings[root] = {folder("b"), folder("a")};
  reader.listings[root / "a"] = {file("one.bin", 500, 7)};
  reader.listings[root / "b"] = {file("same.bin", 500, 7), file("other.bin", 5, 8)};

  const ScanResult result = scan_ok(reader, root, ScanOptions{.workers = 3});

  EXPECT_FALSE(folder_at(result, root / "a").files.front().extra_link);
  const Folder& b = folder_at(result, root / "b");
  EXPECT_FALSE(b.files[0].extra_link);  // other.bin
  EXPECT_TRUE(b.files[1].extra_link);   // same.bin
  EXPECT_EQ(b.size_on_disk, Bytes{5});
  EXPECT_EQ(b.file_count, 2U);
  EXPECT_EQ(result.root().size_on_disk, Bytes{505});
}

TEST(Scanner, StopsWhenCancelled) {
  FakeDirectoryReader reader;
  reader.listings[root] = {folder("a"), folder("b")};
  reader.listings[root / "a"] = {file("x", 1, 1)};
  reader.listings[root / "b"] = {file("y", 1, 2)};
  std::stop_source source;
  reader.on_list = [&source] { source.request_stop(); };

  const Result<ScanResult> result =
      scan(reader, root, ScanOptions{.workers = 1}, source.get_token());

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->cancelled);
  EXPECT_FALSE(reader.was_listed(root / "a"));
  EXPECT_EQ(result->root().folder_count, 2U);
  EXPECT_EQ(result->root().file_count, 0U);
}

TEST(Scanner, ReportsProgress) {
  FakeDirectoryReader reader;
  reader.listings[root] = {file("a", 10, 1), file("b", 20, 2), folder("sub")};
  reader.listings[root / "sub"] = {file("c", 30, 3)};
  std::vector<ScanProgress> reports;

  const Result<ScanResult> result =
      scan(reader, root, ScanOptions{.workers = 1, .progress_every = 1}, {},
           [&reports](const ScanProgress& progress) { reports.push_back(progress); });

  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(reports.size(), 2U);
  EXPECT_EQ(reports.back().folders, 2U);
  EXPECT_EQ(reports.back().files, 4U);
  EXPECT_EQ(reports.back().size_on_disk, Bytes{60});
}

TEST(Scanner, GivesTheSameTreeWithAnyNumberOfWorkers) {
  FakeDirectoryReader reader;
  std::uint8_t id = 1;
  std::vector<ListedEntry> top;
  for (int i = 0; i < 40; ++i) {
    const std::string name = "dir" + std::to_string(i);
    top.push_back(folder(name));
    reader.listings[root / name] = {file("f", 10 + static_cast<std::uint64_t>(i), id++),
                                    folder("inner")};
    reader.listings[root / name / "inner"] = {
        file("g", 1, static_cast<std::uint8_t>((id % 5) + 1))};
  }
  reader.listings[root] = top;

  const ScanResult serial = scan_ok(reader, root, ScanOptions{.workers = 1});
  const ScanResult parallel = scan_ok(reader, root, ScanOptions{.workers = 8});

  EXPECT_EQ(serial.root().size_on_disk, parallel.root().size_on_disk);
  EXPECT_EQ(serial.root().file_count, parallel.root().file_count);
  EXPECT_EQ(parallel.root().folder_count, 80U);
  for (const Folder& folder : serial.folders) {
    const Folder& other = folder_at(parallel, folder.path);
    EXPECT_EQ(folder.size_on_disk, other.size_on_disk) << utf8(folder.path);
    ASSERT_EQ(folder.files.size(), other.files.size());
    for (std::size_t i = 0; i < folder.files.size(); ++i) {
      EXPECT_EQ(folder.files[i].extra_link, other.files[i].extra_link);
    }
  }
}

}  // namespace
}  // namespace myclean::space
