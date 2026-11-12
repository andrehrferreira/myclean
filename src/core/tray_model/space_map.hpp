#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/model/units.hpp"
#include "core/space/treemap.hpp"
#include "core/tray_model/cleaning.hpp"

// The treemap of the Space view (SPC-1): one level of a space.top answer, the folders directly
// inside the folder shown, laid out as rectangles the owner clicks to look inside. Not
// thread-safe; the tray draws the cells.
namespace myclean::tray_model {

struct TreemapCell {
  enum class Kind {
    folder,  // a folder directly inside: `folder` is the id space.top takes to look inside it
    files,   // the files directly inside the folder shown
    other,   // the folders directly inside that the answer does not list
  };
  Kind kind = Kind::folder;
  std::int64_t folder = 0;  // kind folder; 0 when the service gave no id (it cannot be opened)
  std::string label;        // the folder's name, or what the cell holds
  Bytes size_on_disk;
  space::Rect rect;
};

// The cells of `reply` (a space.top answer) inside `bounds`: its children, the files directly
// inside it and the children not listed, each with its share of the area; empty cells are left
// out. Children do not overlap, so nothing is counted twice.
[[nodiscard]] std::vector<TreemapCell> treemap_cells(const SpaceReply& reply,
                                                     const space::Rect& bounds);

// The cell under the point (`x`, `y`); null when there is none.
[[nodiscard]] const TreemapCell* cell_at(const std::vector<TreemapCell>& cells, double x, double y);

// Where the owner is in the treemap: the volume, then each folder opened inside the one before.
class FolderTrail {
 public:
  // Looks inside folder `id`, a cell of the folder shown.
  void open(std::int64_t id);
  // One level up; nothing at the volume.
  void back();
  // To the volume (another volume was chosen, or an id is no longer known).
  void reset();

  // The id of the folder shown; nullopt at the volume.
  [[nodiscard]] std::optional<std::int64_t> current() const;
  [[nodiscard]] bool at_volume() const { return folders_.empty(); }

 private:
  std::vector<std::int64_t> folders_;  // the folders opened, the one shown last
};

}  // namespace myclean::tray_model
