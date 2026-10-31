#include "core/index/volume_index.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/index/indexed_volume.hpp"
#include "core/index/mft.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "ntfs_image.hpp"
#include "tree_model.hpp"

namespace myclean::index {
namespace {

using testing::Entry;
using testing::FakeVolume;
using testing::RecordBuilder;
using testing::ref;
using testing::root_ref;
using testing::TreeModel;

namespace fs = std::filesystem;

// Records 16.. are free for the tree (0 is $MFT, 5 the root).
std::vector<Entry> sample_tree() {
  return {
      {.record = 16, .name = u"Users", .directory = true},
      {.record = 17, .parent = 16, .name = u"alice", .directory = true},
      {.record = 20, .parent = 17, .name = u"notes.txt", .size = 300},
      {.record = 21, .parent = 17, .name = u"video.mp4", .size = 5'000'000, .allocated = 5'001'216},
      {.record = 22,
       .parent = 17,
       .name = u"shared.bin",
       .size = 8192,
       .allocated = 8192,
       .links = {{17, u"copy.bin"}, {31, u"shared.bin"}}},
      {.record = 23, .parent = 17, .name = u"OneDrive", .directory = true},
      {.record = 24,
       .parent = 23,
       .name = u"cloud.docx",
       .size = 50'000,
       .allocated = 4096,
       .attributes = testing::archive | testing::recall_on_data_access,
       .reparse_tag = testing::tag_cloud},
      {.record = 25,
       .parent = 17,
       .name = u"Link",
       .directory = true,
       .reparse_tag = testing::tag_mount_point},
      {.record = 26, .parent = 17, .name = u"sym.txt", .reparse_tag = testing::tag_symlink},
      {.record = 27,
       .parent = 17,
       .name = u"r\u00e9sum\u00e9.pdf",
       .size = 70'000,
       .allocated = 73'728},
      {.record = 30, .name = u"Windows", .directory = true},
      {.record = 31, .parent = 30, .name = u"System32", .directory = true},
      {.record = 32,
       .parent = 31,
       .name = u"kernel.dll",
       .size = 2'000'000,
       .allocated = 2'002'944},
      {.record = 33,
       .parent = 30,
       .name = u"app.dll",
       .size = 300'000,
       .allocated = 90'112,
       .reparse_tag = testing::tag_wof,
       .wof = true},
      {.record = 40, .name = u"Empty", .directory = true},
  };
}

class VolumeIndexTest : public ::testing::Test {
 protected:
  VolumeIndexTest() { model().install(volume()); }

  VolumeIndex build() {
    Result<VolumeIndex> index = read_mft(volume());
    EXPECT_TRUE(index) << index.error().message;
    return std::move(*index);
  }

  space::ScanResult walk(const fs::path& root) {
    Result<space::ScanResult> scan = space::scan(model(), root, space::ScanOptions{.workers = 1});
    EXPECT_TRUE(scan) << scan.error().message;
    return std::move(*scan);
  }

  FakeVolume& volume() { return volume_; }
  TreeModel& model() { return model_; }

 private:
  FakeVolume volume_;
  TreeModel model_{sample_tree()};
};

TEST_F(VolumeIndexTest, GivesTheSameTreeAsScannerV1ForTheWholeVolume) {
  const VolumeIndex index = build();
  const Result<space::ScanResult> tree = index.to_scan_result("C:\\");
  ASSERT_TRUE(tree) << tree.error().message;
  testing::expect_same_tree(*tree, walk("C:\\"));
  EXPECT_EQ(index.size_on_disk(), tree->root().size_on_disk);
  EXPECT_EQ(index.unparented_count(), 0U);
}

TEST_F(VolumeIndexTest, GivesTheSameTreeAsScannerV1ForAFolder) {
  const VolumeIndex index = build();
  const Result<space::ScanResult> tree = index.to_scan_result("c:\\windows\\SYSTEM32");
  ASSERT_TRUE(tree) << tree.error().message;
  testing::expect_same_tree(*tree, walk("C:\\Windows\\System32"));
  // Inside System32 the shared file is the first link seen, so it is counted there.
  EXPECT_EQ(tree->root().size_on_disk, Bytes{2'002'944 + 8192});
}

TEST_F(VolumeIndexTest, CountsAHardLinkedFileOnceAtItsFirstLinkByName) {
  const space::ScanResult tree = build().to_scan_result("C:\\").value();
  std::vector<std::pair<fs::path, bool>> links;  // path, counted
  for (const space::Folder& folder : tree.folders) {
    for (const space::FileItem& file : folder.files) {
      if (file.identity.file_id[0] == 22) {
        links.emplace_back(space::path_of(folder, file), !file.extra_link);
      }
    }
  }
  // Depth-first by name: the files of C:\Users\alice come before C:\Windows, and "copy.bin"
  // before "shared.bin".
  EXPECT_EQ(links,
            (std::vector<std::pair<fs::path, bool>>{{"C:\\Users\\alice\\copy.bin", true},
                                                    {"C:\\Users\\alice\\shared.bin", false},
                                                    {"C:\\Windows\\System32\\shared.bin", false}}));
}

TEST_F(VolumeIndexTest, NeverFollowsALinkAndFlagsAPlaceholder) {
  const space::ScanResult tree = build().to_scan_result("C:\\Users\\alice").value();
  EXPECT_EQ(tree.link_count, 2U);  // the junction and the symbolic link
  EXPECT_EQ(tree.placeholder_count, 1U);
  for (const space::FileItem& file : tree.root().files) {
    if (file.link) {
      EXPECT_EQ(file.size_on_disk, Bytes{});
    }
  }
}

TEST_F(VolumeIndexTest, CountsTheCompressedStreamOfAWindowsCompressedFile) {
  const space::ScanResult tree = build().to_scan_result("C:\\Windows").value();
  EXPECT_EQ(tree.root().own_size_on_disk, Bytes{90'112});
}

TEST_F(VolumeIndexTest, LeavesFilesOutOnRequestButKeepsTheTotals) {
  const VolumeIndex index = build();
  const space::ScanResult full = index.to_scan_result("C:\\").value();
  const space::ScanResult folders = index.to_scan_result("C:\\", false).value();
  ASSERT_EQ(folders.folders.size(), full.folders.size());
  for (std::size_t i = 0; i < full.folders.size(); ++i) {
    EXPECT_TRUE(folders.folders[i].files.empty());
    EXPECT_EQ(folders.folders[i].size_on_disk, full.folders[i].size_on_disk);
    EXPECT_EQ(folders.folders[i].file_count, full.folders[i].file_count);
  }
}

TEST_F(VolumeIndexTest, ListsTheFolderSizesAboveAThreshold) {
  const std::vector<FolderSize> folders = build().folder_sizes(Bytes{2'000'000});
  std::vector<fs::path> paths;
  paths.reserve(folders.size());
  for (const FolderSize& folder : folders) {
    paths.push_back(folder.path);
  }
  EXPECT_EQ(paths, (std::vector<fs::path>{"C:\\", "C:\\Users", "C:\\Users\\alice", "C:\\Windows",
                                          "C:\\Windows\\System32"}));
  EXPECT_EQ(folders[4].size_on_disk, Bytes{2'002'944});  // the hard link counts in Users
}

TEST_F(VolumeIndexTest, RefusesAPathThatIsNotAFolderOfTheVolume) {
  const VolumeIndex index = build();
  EXPECT_EQ(index.to_scan_result("C:\\Nope").error().code, Errc::not_found);
  EXPECT_EQ(index.to_scan_result("D:\\Users").error().code, Errc::not_found);
  EXPECT_EQ(index.to_scan_result("C:\\Users\\alice\\notes.txt").error().code,
            Errc::invalid_argument);
  EXPECT_EQ(index.to_scan_result("C:\\Users\\alice\\Link").error().code, Errc::invalid_argument);
}

TEST_F(VolumeIndexTest, DropsNamesWhoseFolderIsMissingOrUnreachable) {
  volume().put(50, RecordBuilder{50}
                       .standard_information(testing::archive)
                       .file_name(ref(60), u"orphan.txt")
                       .data(u"", 4096, 4096));
  // Two folders that name each other as parent never reach the root.
  volume().put(51, RecordBuilder{51}.directory().file_name(ref(52), u"a"));
  volume().put(52, RecordBuilder{52}.directory().file_name(ref(51), u"b"));
  // A folder name whose parent reference carries an old sequence number.
  volume().put(53, RecordBuilder{53}.directory().file_name(ref(16, 9), u"stale"));
  const VolumeIndex index = build();
  EXPECT_EQ(index.unparented_count(), 4U);
  testing::expect_same_tree(index.to_scan_result("C:\\").value(), walk("C:\\"));
}

TEST_F(VolumeIndexTest, ReadsAnMftWhoseRunsContinueInAnExtensionRecord) {
  const std::vector<Extent>& runs = volume().mft_runs();
  std::vector<std::byte> list = testing::list_entry(0x10, ref(0));
  for (const auto& entry : {testing::list_entry(0x30, ref(0)), testing::list_entry(0x80, ref(0)),
                            testing::list_entry(0x80, ref(15), runs[1].vcn)}) {
    list.insert(list.end(), entry.begin(), entry.end());
  }
  volume().put(0, RecordBuilder{0}
                      .attribute_list(list)
                      .standard_information(0x06)
                      .file_name(root_ref, u"$MFT")
                      .data(u"", 65536, 65536, {runs[0]}));
  volume().put(15, RecordBuilder{15}.base(ref(0)).data_part(runs[1].vcn, {runs[1]}));
  volume().put(45, RecordBuilder{45}
                       .standard_information(testing::archive)
                       .file_name(ref(40), u"late.bin")
                       .data(u"", 4096, 4096));
  const VolumeIndex index = build();
  const space::ScanResult empty = index.to_scan_result("C:\\Empty").value();
  ASSERT_EQ(empty.root().files.size(), 1U);  // record 45 lies in the second run
  EXPECT_EQ(empty.root().files.front().name, fs::path{"late.bin"});
}

TEST_F(VolumeIndexTest, MergesTheNamesAndStreamsOfExtensionRecords) {
  volume().put(41, RecordBuilder{41}
                       .attribute_list(testing::list_entry(0x30, ref(42)))
                       .standard_information(testing::archive)
                       .data(u"", 10'000, 12'288));
  volume().put(42, RecordBuilder{42}.base(ref(41)).file_name(ref(40), u"split.bin"));
  const space::ScanResult empty = build().to_scan_result("C:\\Empty").value();
  ASSERT_EQ(empty.root().files.size(), 1U);
  EXPECT_EQ(empty.root().files.front().name, fs::path{"split.bin"});
  EXPECT_EQ(empty.root().size_on_disk, Bytes{12'288});
}

TEST_F(VolumeIndexTest, FailsWithoutARootFolder) {
  volume().free(root_record);
  const Result<VolumeIndex> index = read_mft(volume());
  ASSERT_FALSE(index);
  EXPECT_EQ(index.error().code, Errc::invalid_data);
}

TEST_F(VolumeIndexTest, StopsWhenAsked) {
  std::stop_source stop;
  stop.request_stop();
  const Result<VolumeIndex> index = read_mft(volume(), stop.get_token());
  ASSERT_FALSE(index);
  EXPECT_EQ(index.error().code, Errc::invalid_state);
}

TEST_F(VolumeIndexTest, EnumeratesTheSameFilesAsTheMft) {
  std::vector<std::string> names;
  ASSERT_TRUE(
      enumerate_files(volume(), [&](const UsnRecord& record) { names.push_back(record.name); }));
  EXPECT_EQ(names.size(), sample_tree().size() + 2);  // and $MFT and the root
  EXPECT_EQ(names.front(), "$MFT");
}

TEST(VolumeIndexMemory, StaysBelow100BytesPerFile) {
  constexpr std::uint32_t folders = 1'000;
  constexpr std::uint32_t files_per_folder = 100;
  VolumeIndex index{"C:\\", testing::serial};
  RecordSegment segment;
  segment.in_use = true;
  segment.sequence = root_record;
  segment.directory = true;
  segment.record = root_record;
  segment.names = {FileName{.parent = root_ref, .name = "."}};
  index.add(segment);
  segment.sequence = 1;
  std::uint64_t record = 16;
  for (std::uint32_t folder = 0; folder < folders; ++folder) {
    const std::uint64_t folder_record = record++;
    segment.record = folder_record;
    segment.directory = true;
    segment.names = {FileName{.parent = root_ref, .name = "folder-" + std::to_string(folder)}};
    index.add(segment);
    segment.directory = false;
    segment.size = 4096;
    segment.size_on_disk = 4096;
    for (std::uint32_t file = 0; file < files_per_folder; ++file) {
      segment.record = record++;
      // A typical name: 20 bytes.
      segment.names = {FileName{.parent = ref(folder_record),
                                .name = "document-" + std::to_string(100'000 + file) + ".pdf"}};
      index.add(segment);
    }
    segment.size.reset();
    segment.size_on_disk = 0;
  }
  ASSERT_TRUE(index.finish());
  const double per_file =
      static_cast<double>(index.memory_bytes()) / static_cast<double>(index.node_count());
  RecordProperty("bytes_per_file", std::to_string(per_file));
  std::cout << "index memory: " << per_file << " bytes per file\n";
  EXPECT_LT(per_file, 100.0);
  EXPECT_EQ(index.size_on_disk(), Bytes{std::uint64_t{folders} * files_per_folder * 4096});
}

// X7, the CPU part of a build: MYCLEAN_X7_SYNTHETIC=1 indexes an in-memory $MFT of 200 000
// records (256 MiB) and prints the rate. No volume is touched; skipped unless set.
TEST(X7, MeasuresTheIndexBuildFromASyntheticMft) {
  std::size_t length = 0;
  std::array<char, 8> value{};
  if (::getenv_s(&length, value.data(), value.size(), "MYCLEAN_X7_SYNTHETIC") != 0 || length == 0) {
    GTEST_SKIP() << "set MYCLEAN_X7_SYNTHETIC=1 to measure the index build";
  }
  constexpr std::uint64_t capacity = 262'144;
  FakeVolume volume{capacity};
  TreeModel{{}}.install(volume);
  std::uint64_t record = 16;
  for (int folder = 0; folder < 2'000; ++folder) {
    const std::uint64_t folder_record = record++;
    volume.put(
        folder_record,
        testing::record_of_entry(
            {.record = folder_record,
             .name = u"folder-" + std::u16string(1, static_cast<char16_t>(u'a' + (folder % 26))),
             .directory = true}));
    for (int file = 0; file < 100; ++file) {
      volume.put(record, testing::record_of_entry({.record = record,
                                                   .parent = folder_record,
                                                   .name = u"document-1234567.pdf",
                                                   .size = 100'000,
                                                   .allocated = 102'400}));
      ++record;
    }
  }
  const auto start = std::chrono::steady_clock::now();
  const VolumeIndex index = read_mft(volume).value();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "X7 synthetic $MFT: " << capacity << " records (" << record - 16
            << " in use) indexed in " << seconds
            << " s = " << static_cast<double>(capacity) / seconds << " records/s, "
            << static_cast<double>(capacity * testing::record_size) / seconds / (1 << 20U)
            << " MiB/s; "
            << static_cast<double>(index.memory_bytes()) / static_cast<double>(index.node_count())
            << " bytes/name\n";
  EXPECT_EQ(index.unparented_count(), 0U);
}
}  // namespace
}  // namespace myclean::index
