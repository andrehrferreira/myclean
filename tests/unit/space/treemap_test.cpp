#include "core/space/treemap.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace myclean::space {
namespace {

constexpr double tolerance = 1e-9;

double aspect(const Rect& rect) {
  return std::max(rect.width, rect.height) / std::min(rect.width, rect.height);
}

double overlap(const Rect& left, const Rect& right) {
  const double width =
      std::min(left.x + left.width, right.x + right.width) - std::max(left.x, right.x);
  const double height =
      std::min(left.y + left.height, right.y + right.height) - std::max(left.y, right.y);
  return width > 0.0 && height > 0.0 ? width * height : 0.0;
}

// Each rectangle is inside `bounds` with its share of the area, and no two overlap.
void expect_tiling(const std::vector<double>& sizes, const std::vector<Rect>& rects,
                   const Rect& bounds) {
  ASSERT_EQ(rects.size(), sizes.size());
  const double sum = std::accumulate(sizes.begin(), sizes.end(), 0.0);
  double covered = 0.0;
  for (std::size_t index = 0; index < rects.size(); ++index) {
    const Rect& rect = rects[index];
    EXPECT_NEAR(rect.area(), sizes[index] / sum * bounds.area(), tolerance * bounds.area())
        << index;
    EXPECT_GE(rect.x, bounds.x - tolerance) << index;
    EXPECT_GE(rect.y, bounds.y - tolerance) << index;
    EXPECT_LE(rect.x + rect.width, bounds.x + bounds.width + tolerance) << index;
    EXPECT_LE(rect.y + rect.height, bounds.y + bounds.height + tolerance) << index;
    for (std::size_t other = index + 1; other < rects.size(); ++other) {
      EXPECT_NEAR(overlap(rect, rects[other]), 0.0, tolerance * bounds.area())
          << index << " and " << other;
    }
    covered += rect.area();
  }
  EXPECT_NEAR(covered, bounds.area(), tolerance * bounds.area());
}

// The example of the paper: a 6 x 4 rectangle and the areas 6, 6, 4, 3, 2, 2, 1.
TEST(Squarify, TilesThePapersExampleWithNearlySquareRectangles) {
  const std::vector<double> sizes{6, 6, 4, 3, 2, 2, 1};
  const Rect bounds{.x = 0, .y = 0, .width = 6, .height = 4};
  const std::vector<Rect> rects = squarify(sizes, bounds);

  expect_tiling(sizes, rects, bounds);
  // The first row is the two 6s stacked on the left, 3 wide and 2 high each.
  EXPECT_NEAR(rects[0].x, 0.0, tolerance);
  EXPECT_NEAR(rects[0].width, 3.0, tolerance);
  EXPECT_NEAR(rects[0].height, 2.0, tolerance);
  EXPECT_NEAR(rects[1].y, 2.0, tolerance);
  for (const Rect& rect : rects) {
    EXPECT_LE(aspect(rect), 3.0);
  }
}

TEST(Squarify, KeepsTheOrderOfTheSizesAndOffsetBounds) {
  const std::vector<double> sizes{1, 10, 3, 7, 0.5, 2};
  const Rect bounds{.x = 100, .y = 50, .width = 320, .height = 180};
  const std::vector<Rect> rects = squarify(sizes, bounds);

  expect_tiling(sizes, rects, bounds);
  // The largest size, second in the input, gets the largest rectangle, laid out first.
  EXPECT_NEAR(rects[1].x, 100.0, tolerance);
  EXPECT_NEAR(rects[1].y, 50.0, tolerance);
}

TEST(Squarify, EqualSizesInASquareBecomeSquares) {
  const std::vector<double> sizes(16, 1.0);
  const Rect bounds{.x = 0, .y = 0, .width = 400, .height = 400};
  const std::vector<Rect> rects = squarify(sizes, bounds);

  expect_tiling(sizes, rects, bounds);
  for (const Rect& rect : rects) {
    EXPECT_NEAR(aspect(rect), 1.0, 1e-6);
  }
}

// Folder sizes span orders of magnitude; the big ones stay close to squares.
TEST(Squarify, SkewedSizesStayNearlySquareWhereTheyAreLarge) {
  std::vector<double> sizes;
  sizes.reserve(40);
  for (int index = 0; index < 40; ++index) {
    sizes.push_back(1000.0 / (index + 1));
  }
  const Rect bounds{.x = 0, .y = 0, .width = 800, .height = 300};
  const std::vector<Rect> rects = squarify(sizes, bounds);

  expect_tiling(sizes, rects, bounds);
  double ratios = 0.0;
  for (std::size_t index = 0; index < rects.size(); ++index) {
    ratios += aspect(rects[index]);
    if (index < 10) {
      EXPECT_LE(aspect(rects[index]), 3.0) << index;
    }
  }
  EXPECT_LE(ratios / static_cast<double>(rects.size()), 2.0);
}

TEST(Squarify, ZeroSizesAndEmptyBoundsGetEmptyRectangles) {
  const Rect bounds{.x = 5, .y = 7, .width = 100, .height = 50};
  const std::vector<double> sizes{0, 4, -1, 4};
  const std::vector<Rect> rects = squarify(sizes, bounds);
  ASSERT_EQ(rects.size(), 4U);
  for (const std::size_t index : {0U, 2U}) {
    EXPECT_EQ(rects[index].area(), 0.0);
    EXPECT_EQ(rects[index].x, 5.0);
    EXPECT_EQ(rects[index].y, 7.0);
  }
  EXPECT_NEAR(rects[1].area() + rects[3].area(), bounds.area(), tolerance);
  EXPECT_NEAR(overlap(rects[1], rects[3]), 0.0, tolerance);

  EXPECT_TRUE(squarify({}, bounds).empty());
  const std::vector<double> none{0, 0};
  for (const Rect& rect : squarify(none, bounds)) {
    EXPECT_EQ(rect.area(), 0.0);
  }
  const std::vector<double> one{3};
  const Rect flat{.x = 0, .y = 0, .width = 100, .height = 0};
  EXPECT_EQ(squarify(one, flat)[0].area(), 0.0);
  const Rect whole = squarify(one, bounds)[0];
  EXPECT_NEAR(whole.width, 100.0, tolerance);
  EXPECT_NEAR(whole.height, 50.0, tolerance);
}

}  // namespace
}  // namespace myclean::space
