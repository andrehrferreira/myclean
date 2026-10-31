#include "core/index/indexed_volume.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/index/mft.hpp"
#include "core/index/usn.hpp"
#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/space/scanner.hpp"
#include "ntfs_image.hpp"
#include "tree_model.hpp"

namespace myclean::index {
namespace {

using testing::Entry;
using testing::FakeVolume;
using testing::ref;
using testing::TreeModel;

namespace fs = std::filesystem;

constexpr std::uint32_t data_extend = 0x00000002;
constexpr std::uint32_t file_create = 0x00000100;
constexpr std::uint32_t rename_new_name = 0x00002000;
constexpr std::uint32_t hard_link_change = 0x00010000;
constexpr std::uint32_t deleted = usn_reason::file_delete | usn_reason::close;

std::vector<Entry> small_tree() {
  return {
      {.record = 16, .name = u"Users", .directory = true},
      {.record = 17, .parent = 16, .name = u"alice", .directory = true},
      {.record = 20, .parent = 17, .name = u"notes.txt", .size = 300},
      {.record = 21, .parent = 17, .name = u"video.mp4", .size = 5'000'000, .allocated = 5'001'216},
      {.record = 22, .parent = 17, .name = u"a.bin", .size = 4096, .allocated = 4096},
      {.record = 23, .parent = 17, .name = u"OneDrive", .directory = true},
      {.record = 24, .parent = 23, .name = u"doc.docx", .size = 40'000, .allocated = 40'960},
      {.record = 30, .name = u"Windows", .directory = true},
      {.record = 31, .parent = 30, .name = u"System32", .directory = true},
      {.record = 32,
       .parent = 31,
       .name = u"kernel.dll",
       .size = 2'000'000,
       .allocated = 2'002'944},
  };
}

class IndexedVolumeTest : public ::testing::Test {
 protected:
  IndexedVolumeTest() {
    model().install(volume());
    Result<IndexedVolume> built = IndexedVolume::build(volume());
    EXPECT_TRUE(built) << built.error().message;
    indexed_.emplace(std::move(*built));
  }

  // Writes `entry` into the volume and journals it.
  void change(const Entry& entry, std::uint32_t reason) {
    volume().put(entry.record, testing::record_of_entry(entry));
    volume().journal_add(ref(entry.record, entry.sequence), testing::parent_ref(entry.parent),
                         reason | usn_reason::close, entry.name);
  }

  RefreshOutcome refresh() {
    Result<RefreshOutcome> outcome = indexed().refresh(volume());
    EXPECT_TRUE(outcome) << outcome.error().message;
    return outcome.value_or(RefreshOutcome{});
  }

  Bytes size_of(const fs::path& folder) {
    return indexed().index().to_scan_result(folder, false).value().root().size_on_disk;
  }

  // The updated index must equal an index read again from the $MFT.
  void expect_matches_rebuild() {
    const VolumeIndex fresh = read_mft(volume()).value();
    testing::expect_same_tree(indexed().index().to_scan_result("C:\\").value(),
                              fresh.to_scan_result("C:\\").value());
    EXPECT_EQ(indexed().index().folder_sizes(Bytes{}), fresh.folder_sizes(Bytes{}));
  }

  FakeVolume& volume() { return volume_; }
  TreeModel& model() { return model_; }
  IndexedVolume& indexed() {
    if (!indexed_.has_value()) {
      throw std::logic_error("the index was not built");
    }
    return *indexed_;
  }
  // The volume's journal (the test volume starts with one).
  JournalState& journal() {
    std::optional<JournalState>& state = volume_.journal_state();
    if (!state.has_value()) {
      throw std::logic_error("the volume has no journal");
    }
    return *state;
  }
  // Where the index resumes reading the journal.
  std::int64_t cursor() {
    const std::optional<JournalState>& state = indexed().journal();
    if (!state.has_value()) {
      throw std::logic_error("the index has no journal cursor");
    }
    return state->next_usn;
  }

 private:
  FakeVolume volume_;
  TreeModel model_{small_tree()};
  std::optional<IndexedVolume> indexed_;
};

TEST_F(IndexedVolumeTest, AddsACreatedFileToEveryFolderAbove) {
  const Bytes users = size_of("C:\\Users");
  change({.record = 60, .parent = 17, .name = u"new.bin", .size = 1 << 20, .allocated = 1 << 20},
         file_create);
  const RefreshOutcome outcome = refresh();
  EXPECT_EQ(outcome.kind, RefreshKind::updated);
  EXPECT_EQ(outcome.records, 1U);
  EXPECT_EQ(size_of("C:\\Users"), users + Bytes{1 << 20});
  EXPECT_EQ(cursor(), journal().next_usn);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, GrowsTheFoldersAboveAFileThatGrew) {
  const Bytes root = indexed().index().size_on_disk();
  change(
      {.record = 21, .parent = 17, .name = u"video.mp4", .size = 9'000'000, .allocated = 9'003'008},
      data_extend);
  refresh();
  EXPECT_EQ(indexed().index().size_on_disk(), root + Bytes{9'003'008 - 5'001'216});
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, MovesAFolderWithItsContentsAndTotals) {
  const Bytes users = size_of("C:\\Users");
  const Bytes windows = size_of("C:\\Windows");
  change({.record = 23, .parent = 30, .name = u"Cloud", .directory = true}, rename_new_name);
  refresh();
  EXPECT_EQ(size_of("C:\\Users") + Bytes{40'960}, users);
  EXPECT_EQ(size_of("C:\\Windows"), windows + Bytes{40'960});
  const space::ScanResult moved = indexed().index().to_scan_result("C:\\Windows\\Cloud").value();
  ASSERT_EQ(moved.root().files.size(), 1U);
  EXPECT_EQ(moved.root().files.front().name, fs::path{"doc.docx"});
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, RemovesADeletedFile) {
  const Bytes alice = size_of("C:\\Users\\alice");
  volume().free(21);
  volume().journal_add(ref(21), ref(17), deleted, u"video.mp4");
  refresh();
  EXPECT_EQ(size_of("C:\\Users\\alice") + Bytes{5'001'216}, alice);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, ReadsANewFolderBeforeTheFileCreatedInIt) {
  // The file's entry comes first in the journal; its folder is read when the file is linked.
  volume().put(61, testing::record_of_entry({.record = 61, .name = u"New", .directory = true}));
  change({.record = 62, .parent = 61, .name = u"f.bin", .size = 8192, .allocated = 8192},
         file_create);
  volume().journal_add(ref(61), testing::root_ref, file_create | usn_reason::close, u"New");
  refresh();
  EXPECT_EQ(size_of("C:\\New"), Bytes{8192});
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, CountsANewHardLinkOnce) {
  const Bytes root = indexed().index().size_on_disk();
  change({.record = 22,
          .parent = 17,
          .name = u"a.bin",
          .size = 4096,
          .allocated = 4096,
          .links = {{31, u"a-link.bin"}}},
         hard_link_change);
  refresh();
  EXPECT_EQ(indexed().index().size_on_disk(), root);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, KeepsTheNewFileOfAReusedRecord) {
  volume().journal_add(ref(20, 1), ref(17), deleted, u"notes.txt");
  change({.record = 20,
          .parent = 31,
          .name = u"other.log",
          .size = 12'288,
          .allocated = 12'288,
          .sequence = 2},
         file_create);
  refresh();
  const space::ScanResult system32 =
      indexed().index().to_scan_result("C:\\Windows\\System32").value();
  EXPECT_EQ(system32.root().files.size(), 2U);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, AppliesAJournalReadInSeveralBuffers) {
  for (std::uint64_t record = 50; record < 55; ++record) {
    change({.record = record,
            .parent = 31,
            .name = u"f" + std::u16string(1, static_cast<char16_t>(u'0' + record - 50)),
            .size = 4096,
            .allocated = 4096},
           file_create);
  }
  EXPECT_EQ(refresh().records, 5U);
  EXPECT_EQ(cursor(), journal().next_usn);
  EXPECT_EQ(refresh().records, 0U);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, RebuildsWhenTheJournalWrappedPastTheCursor) {
  change({.record = 60, .parent = 17, .name = u"x.bin", .size = 4096, .allocated = 4096},
         file_create);
  journal().first_usn = journal().next_usn;
  const RefreshOutcome outcome = refresh();
  EXPECT_EQ(outcome.kind, RefreshKind::rebuilt);
  EXPECT_NE(outcome.reason.find("wrapped"), std::string::npos);
  EXPECT_EQ(cursor(), journal().next_usn);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, RebuildsWhenTheJournalWasRecreated) {
  journal().id = 10;
  const RefreshOutcome outcome = refresh();
  EXPECT_EQ(outcome.kind, RefreshKind::rebuilt);
  EXPECT_NE(outcome.reason.find("recreated"), std::string::npos);
}

TEST_F(IndexedVolumeTest, RebuildsWhenTheJournalWasDeleted) {
  volume().journal_state().reset();
  EXPECT_EQ(refresh().kind, RefreshKind::rebuilt);
  EXPECT_EQ(refresh().kind, RefreshKind::rebuilt);  // and on every refresh while there is none
}

TEST_F(IndexedVolumeTest, RebuildsWhenRecordsAreOverwrittenWhileReading) {
  change({.record = 60, .parent = 17, .name = u"x.bin", .size = 4096, .allocated = 4096},
         file_create);
  volume().overwrite_while_reading();
  EXPECT_EQ(refresh().kind, RefreshKind::rebuilt);
  expect_matches_rebuild();
}

TEST_F(IndexedVolumeTest, RebuildsOnceAQuarterOfTheIndexIsDead) {
  for (const std::uint64_t record : {20, 21, 22, 24, 32}) {
    volume().free(record);
    volume().journal_add(ref(record), ref(17), deleted, u"gone");
  }
  EXPECT_EQ(refresh().kind, RefreshKind::updated);
  EXPECT_GT(indexed().index().dead_count() * 4, indexed().index().node_count());
  const RefreshOutcome outcome = refresh();
  EXPECT_EQ(outcome.kind, RefreshKind::rebuilt);
  EXPECT_EQ(indexed().index().dead_count(), 0U);
}

TEST_F(IndexedVolumeTest, ScanTreeUsesTheIndexForItsVolumeAndScannerV1Otherwise) {
  const Result<space::ScanResult> from_index = scan_tree(&indexed().index(), model(), "c:\\Users");
  ASSERT_TRUE(from_index);
  EXPECT_EQ(from_index->root().path, fs::path{"C:\\Users"});
  EXPECT_EQ(model().listed_count(), 0U) << "the index answers without a directory walk";

  const Result<space::ScanResult> walked = scan_tree(nullptr, model(), "C:\\Users");
  ASSERT_TRUE(walked) << walked.error().message;
  testing::expect_same_tree(*from_index, *walked);
  EXPECT_GT(model().listed_count(), 0U);

  const std::size_t listed = model().listed_count();
  ASSERT_TRUE(scan_tree(&indexed().index(), model(), "D:\\Users"));
  EXPECT_GT(model().listed_count(), listed) << "another volume is walked by scanner v1";
}

}  // namespace
}  // namespace myclean::index
