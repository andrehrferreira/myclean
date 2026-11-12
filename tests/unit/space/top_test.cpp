#include "core/space/top.hpp"

#include <filesystem>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "space_fakes.hpp"

namespace myclean::space {
namespace {

using testing::FakeDirectoryReader;
using testing::file;
using testing::folder;
using testing::junction;
using testing::scan_ok;
namespace fs = std::filesystem;

const fs::path root{L"F:\\projects"};

ScanResult sample() {
  FakeDirectoryReader reader;
  reader.listings[root] = {file("big.iso", 900, 1), folder("a"), folder("b"), junction("link")};
  reader.listings[root / "a"] = {file("x.bin", 300, 2), file("y.bin", 300, 3), folder("deep")};
  reader.listings[root / "a" / "deep"] = {file("z.bin", 50, 4)};
  reader.listings[root / "b"] = {file("copy.bin", 900, 1), file("w.bin", 100, 5)};
  return scan_ok(reader, root);
}

TEST(TopItems, ListsTheLargestFilesAndFoldersBelowTheRoot) {
  const TopItems top = top_items(sample(), 3);

  ASSERT_EQ(top.files.size(), 3U);
  EXPECT_EQ(top.files[0].path, root / "big.iso");
  EXPECT_EQ(top.files[0].size_on_disk, Bytes{900});
  // Equal sizes are ordered by path; copy.bin is a hard link of big.iso and is not repeated.
  EXPECT_EQ(top.files[1].path, root / "a" / "x.bin");
  EXPECT_EQ(top.files[2].path, root / "a" / "y.bin");

  ASSERT_EQ(top.folders.size(), 3U);
  EXPECT_EQ(top.folders[0].path, root / "a");
  EXPECT_EQ(top.folders[0].size_on_disk, Bytes{650});
  EXPECT_EQ(top.folders[0].file_count, 3U);
  EXPECT_EQ(top.folders[1].path, root / "b");
  EXPECT_EQ(top.folders[1].size_on_disk, Bytes{100});
  EXPECT_EQ(top.folders[2].path, root / "a" / "deep");
}

// The direct children do not overlap: with the root's own files they add up to the root.
TEST(TopItems, ListsTheDirectChildrenOfTheRootThatAddUpToIt) {
  const TopItems top = top_items(sample(), 3);

  ASSERT_EQ(top.children.size(), 2U);
  EXPECT_EQ(top.children[0].path, root / "a");
  EXPECT_EQ(top.children[0].size_on_disk, Bytes{650});
  EXPECT_EQ(top.children[1].path, root / "b");
  EXPECT_EQ(top.own_size_on_disk, Bytes{900});
  EXPECT_EQ(top.size_on_disk, Bytes{1650});
  EXPECT_EQ(top.own_size_on_disk + top.children[0].size_on_disk + top.children[1].size_on_disk,
            top.size_on_disk);

  const TopItems one = top_items(sample(), 1);
  ASSERT_EQ(one.children.size(), 1U);
  EXPECT_EQ(one.children[0].path, root / "a");
}

TEST(TopItems, KeepsEveryEntryInsideTheScannedPath) {
  const TopItems top = top_items(sample());

  EXPECT_EQ(top.files.size(), 5U);
  EXPECT_EQ(top.folders.size(), 3U);
  for (const TopFile& item : top.files) {
    EXPECT_EQ(item.path.native().rfind(root.native() + L"\\", 0), 0U);
  }
  for (const TopFolder& item : top.folders) {
    EXPECT_NE(item.path, root);
    EXPECT_EQ(item.path.native().rfind(root.native() + L"\\", 0), 0U);
  }
}

}  // namespace
}  // namespace myclean::space
