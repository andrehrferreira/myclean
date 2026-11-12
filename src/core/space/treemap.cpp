#include "core/space/treemap.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <vector>

namespace myclean::space {

namespace {

// The worst aspect ratio (long side over short side) in a row of rectangles of total area
// `total`, whose largest and smallest areas are `largest` and `smallest`, laid along a side of
// length `side`.
double worst_ratio(double total, double largest, double smallest, double side) {
  const double side_squared = side * side;
  const double total_squared = total * total;
  return std::max(side_squared * largest / total_squared,
                  total_squared / (side_squared * smallest));
}

// The end (one past the last) of the row that starts at `first` of `areas` (largest first) along a
// side of length `side`: the next area joins while the row's worst aspect ratio does not get worse.
std::size_t row_end(const std::vector<double>& areas, std::size_t first, double side) {
  const double largest = areas[first];
  double total = largest;
  double smallest = largest;
  std::size_t end = first + 1;
  for (; end < areas.size(); ++end) {
    if (worst_ratio(total + areas[end], largest, areas[end], side) >
        worst_ratio(total, largest, smallest, side)) {
      break;
    }
    total += areas[end];
    smallest = areas[end];
  }
  return end;
}

}  // namespace

std::vector<Rect> squarify(std::span<const double> sizes, const Rect& bounds) {
  std::vector<Rect> rects(sizes.size(), Rect{.x = bounds.x, .y = bounds.y});
  std::vector<std::size_t> order;
  double sum = 0.0;
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    if (sizes[index] > 0.0) {
      order.push_back(index);
      sum += sizes[index];
    }
  }
  if (order.empty() || bounds.width <= 0.0 || bounds.height <= 0.0) {
    return rects;
  }
  std::ranges::stable_sort(order, std::greater{}, [&](std::size_t index) { return sizes[index]; });
  std::vector<double> areas;  // in the order laid out, largest first
  areas.reserve(order.size());
  for (const std::size_t index : order) {
    areas.push_back(sizes[index] / sum * bounds.area());
  }

  Rect free = bounds;
  for (std::size_t next = 0; next < areas.size();) {
    // A row along the shorter side of what is left: a column when it is wide, else a line.
    const bool wide = free.width >= free.height;
    const double side = wide ? free.height : free.width;
    const std::size_t end = row_end(areas, next, side);
    const double total = std::accumulate(areas.begin() + static_cast<std::ptrdiff_t>(next),
                                         areas.begin() + static_cast<std::ptrdiff_t>(end), 0.0);
    // The last row fills what is left, and the last rectangle of a row the rest of its side, so
    // that rounding leaves no gap.
    const double across = wide ? free.width : free.height;
    const double thickness = end == areas.size() ? across : std::min(total / side, across);
    double offset = 0.0;
    for (std::size_t rank = next; rank < end; ++rank) {
      const double length = rank + 1 == end ? side - offset : areas[rank] / thickness;
      rects[order[rank]] =
          wide ? Rect{.x = free.x, .y = free.y + offset, .width = thickness, .height = length}
               : Rect{.x = free.x + offset, .y = free.y, .width = length, .height = thickness};
      offset += length;
    }
    (wide ? free.x : free.y) += thickness;
    (wide ? free.width : free.height) -= thickness;
    next = end;
  }
  return rects;
}

}  // namespace myclean::space
