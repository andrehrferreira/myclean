#include "core/cleanup/recycle_bin.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes.hpp"
#include "fakes/fake_clock.hpp"

// The aged Recycle Bin items (cleaning spec, "Aged Recycle Bin Items"): the $I records the
// service sizes the plan from, and the action the tray runs over the shell's Recycle Bin.
namespace myclean::cleanup {
namespace {

using namespace myclean::literals;
using namespace std::chrono_literals;
using testing::bin_record;
using testing::FakeRecycleBin;

const WallTime now = std::chrono::sys_days{std::chrono::November / 2 / 2026} + 10h;

TEST(BinRecordTest, ReadsVersion2AsWindows10AndLaterWriteIt) {
  const std::vector<std::uint8_t> data =
      bin_record(2, 44'827'000'000, now - 12 * 24h, LR"(E:\HiveLLM\Synap\target)");
  ASSERT_EQ(data.size(), 28U + (2 * 24));  // 23 code units and the NUL

  // nullopt reads as a record without an original path.
  const BinRecord record = parse_bin_record(data).value_or(BinRecord{});

  EXPECT_EQ(record.size, Bytes{44'827'000'000});
  EXPECT_EQ(record.deleted, now - 12 * 24h);
  EXPECT_EQ(record.original, std::filesystem::path(LR"(E:\HiveLLM\Synap\target)"));
}

TEST(BinRecordTest, ReadsVersion1AsVistaTo8WriteIt) {
  const std::vector<std::uint8_t> data = bin_record(1, 4096, now - 40 * 24h, LR"(C:\old.txt)");
  ASSERT_EQ(data.size(), 544U);

  const BinRecord record = parse_bin_record(data).value_or(BinRecord{});

  EXPECT_EQ(record.size, 4_KiB);
  EXPECT_EQ(record.deleted, now - 40 * 24h);
  EXPECT_EQ(record.original, std::filesystem::path(LR"(C:\old.txt)"));
}

TEST(BinRecordTest, TruncatedAndCorruptRecordsAreIgnored) {
  const std::vector<std::uint8_t> good = bin_record(2, 10, now, LR"(D:\a)");
  const std::vector<std::uint8_t> header(good.begin(), good.begin() + 20);
  EXPECT_FALSE(parse_bin_record(header)) << "shorter than the header";
  const std::vector<std::uint8_t> truncated(good.begin(), good.end() - 1);
  EXPECT_FALSE(parse_bin_record(truncated)) << "the path cut short";
  std::vector<std::uint8_t> longer = good;
  longer.push_back(0);
  EXPECT_FALSE(parse_bin_record(longer)) << "more than the length says";

  std::vector<std::uint8_t> version3 = good;
  version3[0] = 3;
  EXPECT_FALSE(parse_bin_record(version3));
  std::vector<std::uint8_t> no_nul = good;
  no_nul[no_nul.size() - 2] = 'x';
  EXPECT_FALSE(parse_bin_record(no_nul)) << "the path must end with its NUL";
  EXPECT_FALSE(parse_bin_record(bin_record(2, 10, now, L""))) << "no original path";
  std::vector<std::uint8_t> negative = good;
  negative[15] = 0x80;
  EXPECT_FALSE(parse_bin_record(negative)) << "a negative size";
  std::vector<std::uint8_t> before_1970 = good;
  std::fill(before_1970.begin() + 16, before_1970.begin() + 24, std::uint8_t{0});
  EXPECT_FALSE(parse_bin_record(before_1970)) << "a zeroed deletion time";

  const std::vector<std::uint8_t> v1 = bin_record(1, 10, now, LR"(C:\x)");
  const std::vector<std::uint8_t> v1_truncated(v1.begin(), v1.end() - 2);
  EXPECT_FALSE(parse_bin_record(v1_truncated));
}

// Deletion time is what counts: items deleted 30 days ago or earlier, whose content is still
// there.
TEST(BinRecordTest, AgedRecordsCountOnlyOldItemsWithTheirContent) {
  const std::vector<BinFile> files = {
      {.path = LR"(E:\$Recycle.Bin\S-1-5-21-1\$IAAA)",
       .data = bin_record(2, 41'750, now - 31 * 24h, LR"(E:\a\target)"),
       .has_content = true},
      {.path = LR"(E:\$Recycle.Bin\S-1-5-21-1\$IBBB)",
       .data = bin_record(2, 6'500, now - 30 * 24h, LR"(E:\b)"),
       .has_content = true},
      {.path = LR"(E:\$Recycle.Bin\S-1-5-21-1\$ICCC)",
       .data = bin_record(2, 900, now - (30 * 24h) + 1h, LR"(E:\c)"),
       .has_content = true},
      {.path = LR"(E:\$Recycle.Bin\S-1-5-21-1\$IDDD)",
       .data = bin_record(2, 23'500, now - 140 * 24h, LR"(E:\d)"),
       .has_content = false},
      {.path = LR"(C:\$Recycle.Bin\S-1-5-21-1\$IEEE)",
       .data = bin_record(1, 70, now - 400 * 24h, LR"(C:\e)"),
       .has_content = true},
      {.path = LR"(C:\$Recycle.Bin\S-1-5-21-1\$IFFF)", .data = {1, 2, 3}, .has_content = true},
  };

  const AgedItems aged = aged_records(files, now, std::chrono::days{30});

  EXPECT_EQ(aged.count, 3);
  EXPECT_EQ(aged.bytes, Bytes{41'750 + 6'500 + 70});
  EXPECT_EQ(aged_records(files, now, std::chrono::days{500}).bytes, Bytes{});
}

TEST(BinRecordTest, ThePlanItemCarriesTheDays) {
  EXPECT_EQ(recycle_bin_item(30), std::filesystem::path(L"recycle-bin:deleted-30-days-ago"));
  EXPECT_EQ(recycle_bin_days(recycle_bin_item(30)), 30);
  EXPECT_FALSE(recycle_bin_days(L"recycle-bin:deleted--days-ago"));
  EXPECT_FALSE(recycle_bin_days(L"recycle-bin:deleted-030-days-ago"));
  EXPECT_FALSE(recycle_bin_days(L"recycle-bin:deleted-3x-days-ago"));
  EXPECT_FALSE(recycle_bin_days(handler_item("Recycle Bin")));
  EXPECT_FALSE(recycle_bin_days(LR"(E:\$Recycle.Bin)"));
}

class RecycleBinCleanupTest : public ::testing::Test {
 protected:
  RecycleBinCleanupTest() {
    bin_.contents = {
        {.id = R"(E:\$RECYCLE.BIN\S-1\$RAAA)",
         .name = R"(E:\HiveLLM\Synap\target)",
         .deleted = now - 31 * 24h,
         .size = 40_GiB},
        {.id = R"(E:\$RECYCLE.BIN\S-1\$RBBB)",
         .name = R"(E:\UzmiGames\uv-ef618d43)",
         .deleted = now - 45 * 24h,
         .size = 6_GiB},
        {.id = R"(E:\$RECYCLE.BIN\S-1\$RCCC)",
         .name = R"(E:\Assets3D\ChibiKnight\Project)",
         .deleted = now - 29 * 24h,
         .size = 1_MiB},
    };
  }

  RecycleBinCleanup action(std::stop_token stop = {}) {
    return RecycleBinCleanup{bin_, clock_, 30, Tier::automatic, std::move(stop)};
  }

  FakeRecycleBin& bin() { return bin_; }

 private:
  FakeRecycleBin bin_;
  fakes::FakeClock clock_{now};
};

TEST_F(RecycleBinCleanupTest, PreviewIsOneItemSizedByTheItemsDeletedLongEnoughAgo) {
  RecycleBinCleanup cleanup = action();

  const Result<Preview> preview = cleanup.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, recycle_bin_item(30));
  EXPECT_EQ(preview->items[0].size, 46_GiB);
  EXPECT_TRUE(bin().removed.empty());
  const ActionInfo info = cleanup.describe();
  EXPECT_EQ(info.context, ExecutionContext::user);
  EXPECT_EQ(info.tier, Tier::automatic);
}

TEST_F(RecycleBinCleanupTest, ApplyDeletesOnlyTheAgedItemsAndCannotBeUndone) {
  RecycleBinCleanup cleanup = action();
  Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);
  preview->items[0].id = PlanItemId{9};

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(bin().removed, (std::vector<std::string>{R"(E:\$RECYCLE.BIN\S-1\$RAAA)",
                                                     R"(E:\$RECYCLE.BIN\S-1\$RBBB)"}));
  ASSERT_EQ(bin().contents.size(), 1U);
  EXPECT_EQ(bin().contents[0].name, R"(E:\Assets3D\ChibiKnight\Project)");
  EXPECT_EQ(undo->not_reversible, "deleted permanently");
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(undo->items[0].item, PlanItemId{9});
  EXPECT_EQ(undo->items[0].data, "2 items");

  const Result<Verification> verified = cleanup.verify(*undo);
  ASSERT_TRUE(verified) << verified.error().message;
  EXPECT_EQ(verified->metric, "recycle_bin_aged_bytes");
  EXPECT_EQ(verified->before, static_cast<double>((46_GiB).count()));
  EXPECT_EQ(verified->after, 0.0);
  EXPECT_EQ(verified->effect, Effect::better);
  EXPECT_EQ(cleanup.undo(*undo).error().code, Errc::not_reversible);
}

TEST_F(RecycleBinCleanupTest, AnItemTheShellKeepsIsSkippedAndCountsAsLeft) {
  bin().failures.insert_or_assign(R"(E:\$RECYCLE.BIN\S-1\$RBBB)",
                                  Error{.code = Errc::io_error, .message = "in use"});
  RecycleBinCleanup cleanup = action();
  Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(undo->items[0].data, "1 items");
  ASSERT_EQ(cleanup.skipped().size(), 1U);
  EXPECT_EQ(cleanup.skipped()[0].target, R"(E:\UzmiGames\uv-ef618d43)");
  const Result<Verification> verified = cleanup.verify(*undo);
  ASSERT_TRUE(verified);
  EXPECT_EQ(verified->after, static_cast<double>((6_GiB).count()));
}

TEST_F(RecycleBinCleanupTest, NothingDeletedFailsWithTheFirstReason) {
  bin().failures.insert_or_assign(R"(E:\$RECYCLE.BIN\S-1\$RAAA)",
                                  Error{.code = Errc::access_denied, .message = "denied"});
  bin().failures.insert_or_assign(R"(E:\$RECYCLE.BIN\S-1\$RBBB)",
                                  Error{.code = Errc::io_error, .message = "in use"});
  RecycleBinCleanup cleanup = action();
  Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::access_denied);
}

TEST_F(RecycleBinCleanupTest, NothingOldEnoughPreviewsNothingAndDeletesNothing) {
  bin().contents.erase(bin().contents.begin(), bin().contents.begin() + 2);
  RecycleBinCleanup cleanup = action();

  const Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);
  EXPECT_TRUE(preview->items.empty());
  const Result<Undo> undo = cleanup.apply(*preview);
  ASSERT_TRUE(undo);
  EXPECT_TRUE(undo->items.empty());
  EXPECT_TRUE(bin().removed.empty());
}

TEST_F(RecycleBinCleanupTest, AnUnreadableBinFailsThePreview) {
  bin().failure = Error{.code = Errc::io_error, .message = "SHGetKnownFolderItem failed"};
  RecycleBinCleanup cleanup = action();
  EXPECT_EQ(cleanup.preview().error().code, Errc::io_error);
}

}  // namespace
}  // namespace myclean::cleanup
