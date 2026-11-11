// RecycleItems over FakeFileSystem and a fake recycler: only the file the plan recorded goes to
// the Recycle Bin, never one in a deny-list place, a link, a cloud file or a file swapped in since.

#include "core/user_tasks/recycle.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../cleanup/fakes.hpp"
#include "core/cleanup/handler_cleanup.hpp"
#include "core/cleanup/recycle_bin.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::user_tasks {
namespace {

namespace fs = std::filesystem;
using namespace myclean::literals;

const fs::path big{L"F:\\big.iso"};
const fs::path copy{L"F:\\Downloads\\copy.mkv"};

class RecycleItemsTest : public ::testing::Test {
 protected:
  RecycleItemsTest()
      : locations_{.windows = L"C:\\Windows",
                   .profiles = L"C:\\Users",
                   .owner_profile = L"C:\\Users\\owner",
                   .owner_known_folders = {L"C:\\Users\\owner\\Documents"},
                   .myclean = {L"C:\\ProgramData\\MyClean"}} {
    recycler_.on_recycle = [this](const fs::path& file) { files_.erase(file); };
  }

  [[nodiscard]] RecycleItem item(std::int64_t id, const fs::path& path, Bytes size) {
    return RecycleItem{
        .id = PlanItemId{id}, .path = path, .identity = files_.add_file(path, size), .size = size};
  }
  [[nodiscard]] RecycleItems action(std::vector<RecycleItem> items) {
    return RecycleItems(
        RecycleTools{.recycler = &recycler_, .file_system = &files_, .locations = &locations_},
        std::move(items));
  }

  [[nodiscard]] fakes::FakeFileSystem& files() { return files_; }
  [[nodiscard]] cleanup::testing::FakeRecycler& recycler() { return recycler_; }

 private:
  fakes::FakeFileSystem files_;
  cleanup::testing::FakeRecycler recycler_;
  safety::ProtectedLocations locations_;
};

TEST_F(RecycleItemsTest, SendsTheRecordedFilesAndVerifiesThemGone) {
  RecycleItems recycle = action({item(1, big, 4_GiB), item(2, copy, 1_GiB)});
  EXPECT_EQ(recycle.describe().context, ExecutionContext::user);

  const Result<Preview> preview = recycle.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 2U);
  EXPECT_EQ(preview->total_size(), 5_GiB);

  const Result<Undo> undo = recycle.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  EXPECT_EQ(recycler().sent, (std::vector<fs::path>{big, copy}));
  ASSERT_EQ(undo->items.size(), 2U);
  EXPECT_EQ(undo->items[0].data, "F:\\big.iso");
  EXPECT_FALSE(undo->reversible());
  EXPECT_EQ(undo->not_reversible, "restore them from the Recycle Bin");

  const Result<Verification> verification = recycle.verify(*undo);
  ASSERT_TRUE(verification.has_value()) << verification.error().message;
  EXPECT_EQ(verification->metric, "files_left");
  EXPECT_EQ(verification->before, 2.0);
  EXPECT_EQ(verification->after, 0.0);
  EXPECT_EQ(verification->effect, Effect::better);
  const Result<void> undone = recycle.undo(*undo);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::not_reversible);
}

TEST_F(RecycleItemsTest, LeavesOutDenyListedLinkedCloudAndSwappedFiles) {
  const fs::path document{L"C:\\Users\\owner\\Documents\\thesis.docx"};
  const fs::path link{L"F:\\link.iso"};
  const fs::path cloud{L"F:\\OneDrive\\video.mp4"};
  const fs::path swapped{L"F:\\swapped.bin"};
  std::vector<RecycleItem> items = {item(1, document, 1_MiB), item(2, big, 4_GiB),
                                    item(4, swapped, 1_MiB)};
  files().add_file(link, 1_MiB, {.reparse_point = true});
  items.push_back(RecycleItem{
      .id = PlanItemId{3}, .path = link, .identity = files().stat(link)->identity, .size = 1_MiB});
  files().add_file(cloud, 1_GiB, {.recall_on_data_access = true});
  items.push_back(RecycleItem{.id = PlanItemId{5},
                              .path = cloud,
                              .identity = files().stat(cloud)->identity,
                              .size = 1_GiB});
  files().erase(swapped);
  files().add_file(swapped, 1_MiB);  // another file under the same name
  items.push_back(RecycleItem{.id = PlanItemId{6}, .path = copy, .size = 1_MiB});  // no identity
  RecycleItems recycle = action(std::move(items));

  const Result<Preview> preview = recycle.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, big);
  ASSERT_EQ(recycle.skipped().size(), 5U);
  EXPECT_EQ(recycle.skipped()[0].error.message, "deny_list:user_known_folder");
  EXPECT_EQ(recycle.skipped()[1].error.code, Errc::refused);  // the swapped file

  ASSERT_TRUE(recycle.apply(*preview).has_value());
  EXPECT_EQ(recycler().sent, std::vector<fs::path>{big});
  EXPECT_TRUE(files().exists(document));
  EXPECT_TRUE(files().exists(link));
  EXPECT_TRUE(files().exists(cloud));
  EXPECT_TRUE(files().exists(swapped));
}

TEST_F(RecycleItemsTest, AFileChangedAfterThePreviewOrRefusedByTheShellStays) {
  RecycleItems recycle = action({item(1, big, 4_GiB), item(2, copy, 1_GiB)});
  const Result<Preview> preview = recycle.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  files().erase(big);
  files().add_file(big, 4_GiB);  // swapped between the preview and apply
  recycler().failures.insert_or_assign(
      copy, Error{.code = Errc::refused, .message = "the Recycle Bin cannot take it"});

  const Result<Undo> undo = recycle.apply(*preview);
  ASSERT_FALSE(undo.has_value());
  EXPECT_NE(undo.error().message.find("cannot take it"), std::string::npos) << undo.error().message;
  EXPECT_EQ(recycler().sent, std::vector<fs::path>{copy});
  EXPECT_TRUE(files().exists(big));
  EXPECT_TRUE(files().exists(copy));
}

}  // namespace
}  // namespace myclean::user_tasks
