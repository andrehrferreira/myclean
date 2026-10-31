#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean {
namespace {

using namespace myclean::literals;

// A minimal action over regenerable items: it records what apply touched, and its undo
// declares that the change is not reversible.
class RecordingDelete final : public Action {
 public:
  explicit RecordingDelete(std::vector<PlanItem> candidates) : candidates_(std::move(candidates)) {}

  [[nodiscard]] ActionInfo describe() const override {
    return {.what = "Delete regenerable caches",
            .why = "Volume C: is almost full",
            .risk = Risk::low,
            .tier = Tier::automatic,
            .context = ExecutionContext::service};
  }

  [[nodiscard]] Result<Preview> preview() override { return Preview{.items = candidates_}; }

  [[nodiscard]] Result<Undo> apply(const Preview& preview) override {
    Undo undo{.not_reversible = "regenerates"};
    for (const PlanItem& item : preview.items) {
      touched_.push_back(item.id);
      undo.items.push_back({.item = item.id, .data = "deleted"});
    }
    return undo;
  }

  [[nodiscard]] Result<Verification> verify(const Undo& /*undo*/) override {
    return Verification{
        .metric = "volume.free", .before = 1.0, .after = 2.0, .effect = Effect::better};
  }

  [[nodiscard]] Result<void> undo(const Undo& undo) override {
    return fail(Errc::not_reversible, "not reversible: " + undo.not_reversible);
  }

  [[nodiscard]] const std::vector<PlanItemId>& touched() const { return touched_; }

 private:
  std::vector<PlanItem> candidates_;
  std::vector<PlanItemId> touched_;
};

TEST(ActionContract, ApplyActsOnlyOnPreviewItemsAndCoversEachInUndo) {
  RecordingDelete action{{PlanItem{.id = PlanItemId{1}, .path = L"C:\\c\\1", .size = 400_MiB},
                          PlanItem{.id = PlanItemId{2}, .path = L"C:\\c\\2", .size = 400_MiB},
                          PlanItem{.id = PlanItemId{3}, .path = L"C:\\c\\3", .size = 400_MiB}}};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->total_size(), 1200_MiB);

  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo.has_value());
  EXPECT_EQ(action.touched(),
            (std::vector<PlanItemId>{PlanItemId{1}, PlanItemId{2}, PlanItemId{3}}));
  ASSERT_EQ(undo->items.size(), 3U);
  EXPECT_EQ(undo->items[1].item, PlanItemId{2});
}

TEST(ActionContract, IrreversibleUndoStatesTheReason) {
  const std::unique_ptr<Action> action = std::make_unique<RecordingDelete>(std::vector<PlanItem>{});
  EXPECT_EQ(action->describe().tier, Tier::automatic);
  const Result<Undo> undo = action->apply(Preview{});
  ASSERT_TRUE(undo.has_value());
  EXPECT_FALSE(undo->reversible());

  const Result<void> result = action->undo(*undo);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, Errc::not_reversible);
  EXPECT_EQ(result.error().message, "not reversible: regenerates");
}

}  // namespace
}  // namespace myclean
