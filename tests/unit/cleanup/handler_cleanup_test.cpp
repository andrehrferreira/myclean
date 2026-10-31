#include "core/cleanup/handler_cleanup.hpp"

#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "fakes.hpp"

namespace myclean::cleanup {
namespace {

using namespace myclean::literals;
using testing::FakeHost;

class HandlerCleanupTest : public ::testing::Test {
 protected:
  HandlerCleanup action(std::vector<std::string> handlers, std::stop_token stop = {}) {
    return HandlerCleanup{
        host_, "C:\\", std::move(handlers), Tier::ask, ExecutionContext::service, std::move(stop)};
  }

  FakeHost& host() { return host_; }

 private:
  FakeHost host_;
};

TEST_F(HandlerCleanupTest, PreviewListsHandlersThatCanFreeSpace) {
  host().sizes.insert_or_assign("Delivery Optimization Files", 3_MiB);
  host().sizes.insert_or_assign("Old ChkDsk Files", Bytes{});
  host().sizes.insert_or_assign("BranchCache",
                                fail(Errc::access_denied, "Initialize failed with 0x80070005"));
  HandlerCleanup cleanup =
      action({"Delivery Optimization Files", "Old ChkDsk Files", "BranchCache"});

  const Result<Preview> preview = cleanup.preview();

  ASSERT_TRUE(preview) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  EXPECT_EQ(preview->items[0].path, handler_item("Delivery Optimization Files"));
  EXPECT_EQ(preview->items[0].size, 3_MiB);
  ASSERT_EQ(cleanup.skipped().size(), 1U);
  EXPECT_EQ(cleanup.skipped()[0].target, "BranchCache");
  EXPECT_EQ(cleanup.skipped()[0].error.code, Errc::access_denied);
  EXPECT_TRUE(host().purged.empty());
  EXPECT_EQ(host().volumes.front(), "C:\\");
}

TEST_F(HandlerCleanupTest, PreviewFailsWhenEveryHandlerFails) {
  host().sizes.insert_or_assign("BranchCache", fail(Errc::access_denied, "denied"));
  HandlerCleanup cleanup = action({"BranchCache"});

  const Result<Preview> preview = cleanup.preview();

  ASSERT_FALSE(preview);
  EXPECT_EQ(preview.error().code, Errc::access_denied);
}

TEST_F(HandlerCleanupTest, ApplyPurgesAndVerifyRescans) {
  host().sizes.insert_or_assign("Windows Error Reporting Files", 6_MiB);
  host().sizes.insert_or_assign("Thumbnail Cache", 2_MiB);
  HandlerCleanup cleanup = action({"Windows Error Reporting Files", "Thumbnail Cache"});
  Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);
  preview->items[0].id = PlanItemId{7};

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_TRUE(undo) << undo.error().message;
  EXPECT_EQ(host().purged,
            (std::vector<std::string>{"Windows Error Reporting Files", "Thumbnail Cache"}));
  EXPECT_EQ(undo->not_reversible, "regenerates");
  ASSERT_EQ(undo->items.size(), 2U);
  EXPECT_EQ(undo->items[0].item, PlanItemId{7});
  EXPECT_EQ(undo->items[0].data, "Windows Error Reporting Files");

  const Result<Verification> verification = cleanup.verify(*undo);
  ASSERT_TRUE(verification);
  EXPECT_EQ(verification->metric, "handler_bytes_reclaimable");
  EXPECT_EQ(verification->before, static_cast<double>((8_MiB).count()));
  EXPECT_EQ(verification->after, 0.0);
  EXPECT_EQ(verification->effect, Effect::better);

  const Result<void> undone = cleanup.undo(*undo);
  ASSERT_FALSE(undone);
  EXPECT_EQ(undone.error().code, Errc::not_reversible);
  EXPECT_EQ(undone.error().message, "not reversible: regenerates");
}

TEST_F(HandlerCleanupTest, ApplyActsOnlyOnItsOwnHandlers) {
  host().sizes.insert_or_assign("Temporary Files", 1_MiB);
  HandlerCleanup cleanup = action({"Temporary Files"});
  const Preview foreign{.items = {PlanItem{.path = handler_item("DownloadsFolder"), .size = 9_GiB},
                                  PlanItem{.path = "C:\\Windows\\Temp", .size = 1_MiB}}};

  const Result<Undo> undo = cleanup.apply(foreign);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().code, Errc::refused);
  EXPECT_TRUE(host().purged.empty());
  EXPECT_EQ(cleanup.skipped().size(), 2U);
}

TEST_F(HandlerCleanupTest, OneFailedPurgeDoesNotStopTheOthers) {
  host().sizes.insert_or_assign("Recycle Bin", 1_MiB);
  host().sizes.insert_or_assign("Thumbnail Cache", 2_MiB);
  host().failures.insert_or_assign("Recycle Bin",
                                   Error{.code = Errc::io_error, .message = "Purge failed"});
  HandlerCleanup cleanup = action({"Recycle Bin", "Thumbnail Cache"});
  const Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_TRUE(undo);
  ASSERT_EQ(undo->items.size(), 1U);
  EXPECT_EQ(undo->items[0].data, "Thumbnail Cache");
  ASSERT_EQ(cleanup.skipped().size(), 1U);
  EXPECT_EQ(cleanup.skipped()[0].target, "Recycle Bin");
}

TEST_F(HandlerCleanupTest, StopRequestPurgesNothing) {
  host().sizes.insert_or_assign("Thumbnail Cache", 2_MiB);
  std::stop_source source;
  HandlerCleanup cleanup = action({"Thumbnail Cache"}, source.get_token());
  const Result<Preview> preview = cleanup.preview();
  ASSERT_TRUE(preview);
  source.request_stop();

  const Result<Undo> undo = cleanup.apply(*preview);

  ASSERT_FALSE(undo);
  EXPECT_EQ(undo.error().message, "cancelled");
  EXPECT_TRUE(host().purged.empty());
}

TEST_F(HandlerCleanupTest, DescribeCarriesTierAndContext) {
  const HandlerCleanup cleanup{
      host(), "C:\\", {"Thumbnail Cache"}, Tier::automatic, ExecutionContext::user};

  const ActionInfo info = cleanup.describe();

  EXPECT_EQ(info.tier, Tier::automatic);
  EXPECT_EQ(info.context, ExecutionContext::user);
  EXPECT_EQ(info.what, "Run the Windows cleanup handlers Thumbnail Cache on C:\\");
}

TEST(HandlerItem, IsNotAnAbsolutePath) {
  EXPECT_FALSE(handler_item("Temporary Files").is_absolute());
}

}  // namespace
}  // namespace myclean::cleanup
