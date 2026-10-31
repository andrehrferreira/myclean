#include "core/drives/layout.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/units.hpp"
#include "drives_fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::drives {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::FakeLayout;
using testing::FakeReader;
using testing::file;
using testing::folder;
using testing::scattered;

const WallTime start = std::chrono::sys_days{std::chrono::October / 31 / 2026};

TEST(CountFragments, JoinsContiguousRunsAndSkipsRunsWithoutClusters) {
  EXPECT_EQ(count_fragments({}), 0U);
  EXPECT_EQ(count_fragments(std::vector<Extent>{{.lcn = -1, .clusters = 16}}), 0U);
  // 100-115 and 116-131 are one piece; the hole and the jump to 500 start new ones.
  const std::vector<Extent> runs = {{.lcn = 100, .clusters = 16},
                                    {.lcn = 116, .clusters = 16},
                                    {.lcn = -1, .clusters = 16},
                                    {.lcn = 500, .clusters = 4},
                                    {.lcn = 10, .clusters = 1}};
  EXPECT_EQ(count_fragments(runs), 3U);
}

TEST(Fragmented, NeedsMoreThanEightFragmentsAndMoreThanEightPerGibibyte) {
  EXPECT_FALSE(fragmented(8, 100_MiB));
  EXPECT_TRUE(fragmented(9, 100_MiB));
  EXPECT_FALSE(fragmented(16, 2_GiB));  // 8 per GiB: written in large pieces
  EXPECT_TRUE(fragmented(17, 2_GiB));
}

TEST(FreeSpaceTally, FindsFreeRunsAcrossBytesAndPieces) {
  // Clusters 0-7 free, 8-19 used, 20-27 free, 28-31 used (least significant bit first).
  const std::vector<std::uint8_t> bitmap = {0x00, 0xFF, 0x0F, 0xF0};
  FreeSpaceTally whole{8_MiB};
  whole.add(bitmap, 32);
  FreeSpaceTally pieces{8_MiB};
  pieces.add(std::vector<std::uint8_t>{0x00, 0xFF, 0x0F}, 24);
  pieces.add(std::vector<std::uint8_t>{0xF0}, 8);
  for (const FreeSpaceLayout& layout : {whole.finish(true), pieces.finish(true)}) {
    EXPECT_EQ(layout.free, 128_MiB);
    EXPECT_EQ(layout.free_runs, 2U);
    EXPECT_EQ(layout.largest_free_run, 64_MiB);
    EXPECT_EQ(layout.free_in_small_runs, 0_B);
    EXPECT_TRUE(layout.complete);
  }
}

TEST(FreeSpaceTally, CountsSmallRunsAndStopsAtTheLastCluster) {
  // 0x55: every other cluster free; only the first 5 bits are clusters of the volume.
  FreeSpaceTally tally{4_KiB};
  tally.add(std::vector<std::uint8_t>{0x55}, 5);
  const FreeSpaceLayout layout = tally.finish(false);
  EXPECT_EQ(layout.free, 8_KiB);  // clusters 1 and 3
  EXPECT_EQ(layout.free_runs, 2U);
  EXPECT_EQ(layout.free_in_small_runs, 8_KiB);
  EXPECT_FALSE(layout.complete);
}

struct SampleTest : public ::testing::Test {
  SampleTest() {
    reader.listings[L"K:\\"] = {
        file("big-fragmented.bin", (200_MiB).count()),
        file("big-whole.bin", (1_GiB).count()),
        file("small.txt", (1_MiB).count()),
        folder("Backups"),
        {.name = L"link", .directory = true, .attributes = {.reparse_point = true}},
        {.name = L"cloud.vhdx",
         .attributes = {.recall_on_data_access = true},
         .size = 5_GiB,
         .size_on_disk = 5_GiB},
        file("locked.bin", (100_MiB).count())};
    reader.listings[L"K:\\Backups"] = {file("image.vhdx", (4_GiB).count())};
    layout.runs[L"K:\\big-fragmented.bin"] = scattered(40);
    layout.runs[L"K:\\big-whole.bin"] = scattered(1);
    layout.runs[L"K:\\Backups\\image.vhdx"] = scattered(64);
  }

  FakeReader reader;
  FakeLayout layout;
  fakes::FakeClock clock{start};
};

TEST_F(SampleTest, ReadsLargeFilesOnlyAndSkipsLinksAndPlaceholders) {
  const FragmentationSample sample =
      sample_fragmentation(L"K:\\", reader, layout, clock, SampleBudget{});
  EXPECT_EQ(sample.folders, 2U);  // the root and Backups; the link is not followed
  EXPECT_EQ(sample.files, 3U);    // small.txt too small, cloud.vhdx never opened
  EXPECT_EQ(sample.errors, 1U);   // locked.bin
  EXPECT_EQ(sample.bytes, 200_MiB + 1_GiB + 4_GiB);
  EXPECT_EQ(sample.fragments, 105U);
  // 40 fragments in 200 MiB and 64 in 4 GiB (16 per GiB) are fragmented; 1 piece is not.
  EXPECT_EQ(sample.fragmented_files, 2U);
  EXPECT_EQ(sample.fragmented_bytes, 200_MiB + 4_GiB);
  EXPECT_NEAR(sample.fragmented_percent(), 100.0 * (4096 + 200) / (4096 + 200 + 1024), 1e-9);
  EXPECT_DOUBLE_EQ(sample.fragments_per_file(), 35.0);
  ASSERT_TRUE(sample.worst.has_value());
  EXPECT_EQ(sample.worst.value_or(SampledFile{}).path,
            std::filesystem::path{L"K:\\Backups\\image.vhdx"});
  EXPECT_FALSE(sample.stopped);
}

TEST_F(SampleTest, StopsWhenTheFileOrTimeBudgetRunsOut) {
  const FragmentationSample by_files =
      sample_fragmentation(L"K:\\", reader, layout, clock, SampleBudget{.max_files = 1});
  EXPECT_EQ(by_files.files, 1U);
  EXPECT_TRUE(by_files.stopped);

  reader.on_list = [this] { clock.advance(20s); };
  const FragmentationSample by_time =
      sample_fragmentation(L"K:\\", reader, layout, clock, SampleBudget{.max_time = 10s});
  EXPECT_EQ(by_time.folders, 1U);
  EXPECT_EQ(by_time.files, 0U);
  EXPECT_TRUE(by_time.stopped);
}

}  // namespace
}  // namespace myclean::drives
