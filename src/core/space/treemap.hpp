#pragma once

#include <span>
#include <vector>

// The squarified treemap layout (Bruls, Huizing and van Wijk, "Squarified Treemaps", 2000) of the
// space spec's treemap: sizes in, rectangles out. Pure geometry; the tray draws the rectangles.
namespace myclean::space {

struct Rect {
  double x = 0.0;  // left
  double y = 0.0;  // top
  double width = 0.0;
  double height = 0.0;

  [[nodiscard]] double area() const { return width * height; }
};

// One rectangle per entry of `sizes`, in the same order, tiling `bounds`: each one's area is its
// share of the sum of `sizes` times the area of `bounds`, and no two overlap. The largest sizes
// are laid out first, in rows along the shorter side of the space left, each row kept as long as
// adding the next size does not make its worst aspect ratio worse, so the rectangles stay close
// to squares. A size of zero or less, or any size when `bounds` has no area, gets an empty
// rectangle at the top-left corner of `bounds`.
[[nodiscard]] std::vector<Rect> squarify(std::span<const double> sizes, const Rect& bounds);

}  // namespace myclean::space
